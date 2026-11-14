// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Copies of the engine's memory, for a page that cannot read it in place: a
// page whose engine renders in a worklet with no shared memory (postMessage
// mode), or one whose engine runs in its host (a plugin's web view: the
// `copies` mode). clockwork_client_copy makes them, so every host hands over
// the same bytes, and this reads them (src/clockwork_client.h, "Copies").
//
// A copy is a header, then the parts asked for and a window per want, each on
// an 8-byte boundary:
//
//   header   eight u32: magic, version, bytes, the arena's generation word,
//            the arena part's bytes, the metrics part's bytes, windows, the
//            parameter table's bytes
//   arena    the arena's header and tables (arena.js readArena reads them)
//   metrics  the metrics, the guest's window, the metrics history, back to
//            back: what postMessage mode's snapshot has always been
//   params   the parameter table's region (params.js reads it), when asked for
//   window   six u32 — source, slot, channels, frames, activations, 0 — and
//            the write position (u64), then frames * channels floats
//
// A page asks for the windows it is drawing: CopyReader keeps what it asked
// for lately, so the host copies those and no others.

export const COPY = Object.freeze({ MAGIC: 0x59504F43, VERSION: 1, HEADER_BYTES: 32, WINDOW_HEADER_BYTES: 32 });
export const COPY_PART = Object.freeze({ ARENA: 1, METRICS: 2, PARAMS: 4 });
export const COPY_SOURCE = Object.freeze({ TAP: 1, SCOPE: 2 });

const align8 = (n) => (n + 7) & ~7;

/** Wants as clockwork_client_copy reads them (ClockworkCopyWant): source, slot, frames, a u32 each. */
export function encodeWants(wants) {
  const out = new Uint32Array(wants.length * 3);
  wants.forEach(({ source, slot, frames }, i) => out.set([source, slot, frames], i * 3));
  return out;
}

/**
 * A copy, read. Each window is the shape tap_slot.js readTapSlot returns, or
 * null for a slot with nothing to show; `activations` is the slot's count of
 * live spells either way. Throws for bytes that are not a copy this reader knows.
 * @param {ArrayBuffer} buffer
 * @returns {{ generation: number, arena: ArrayBuffer|null, metrics: ArrayBuffer|null, params: ArrayBuffer|null,
 *             windows: Array<{ source: number, slot: number, activations: number, writePosition: bigint,
 *                              window: { frames: number, channels: number, writePosition: bigint, interleaved: Float32Array }|null }> }}
 */
export function readCopy(buffer) {
  if (buffer.byteLength < COPY.HEADER_BYTES) throw new Error(`a copy is at least ${COPY.HEADER_BYTES} bytes, not ${buffer.byteLength}`);
  const [magic, version, bytes, generation, arenaBytes, metricsBytes, count, paramsBytes] = new Uint32Array(buffer, 0, 8);
  if (magic !== COPY.MAGIC) throw new Error(`not a copy of an engine (magic ${magic.toString(16)})`);
  if (version !== COPY.VERSION) throw new Error(`a copy of version ${version}, this reader knows ${COPY.VERSION}`);
  if (bytes > buffer.byteLength) throw new Error(`a copy of ${bytes} bytes in ${buffer.byteLength}`);
  let at = COPY.HEADER_BYTES;
  const part = (n) => {
    if (!n) return null;
    if (at + n > bytes) throw new Error("a copy's part runs past its end");
    const out = buffer.slice(at, at + n);
    at = align8(at + n);
    return out;
  };
  const arena = part(arenaBytes);
  const metrics = part(metricsBytes);
  const params = part(paramsBytes);
  const windows = [];
  for (let i = 0; i < count; i++) {
    if (at + COPY.WINDOW_HEADER_BYTES > bytes) throw new Error("a copy's window runs past its end");
    const [source, slot, channels, frames, activations] = new Uint32Array(buffer, at, 6);
    const writePosition = new BigUint64Array(buffer, at + 24, 1)[0];
    at += COPY.WINDOW_HEADER_BYTES;
    const floats = channels * frames;
    if (at + floats * 4 > bytes) throw new Error("a copy's window runs past its end");
    const window = channels ? { frames, channels, writePosition, interleaved: new Float32Array(buffer, at, floats) } : null;
    windows.push({ source, slot, activations, writePosition, window });
    at = align8(at + floats * 4);
  }
  return { generation, arena, metrics, params, windows };
}

/** The newest `frames` of a window holding more. */
function newest(window, frames) {
  if (!window || frames >= window.frames) return window;
  const { channels } = window;
  return { frames, channels, writePosition: window.writePosition, interleaved: window.interleaved.subarray((window.frames - frames) * channels) };
}

/**
 * What a page reads of an engine it has no view of: the newest copy's
 * windows and metrics, and the wants that say which windows the next copy
 * should hold. A window not asked for in `wantFor` ms is no longer copied.
 */
export class CopyReader {
  #wants = new Map();     // `${source}:${slot}` → { source, slot, frames, asked }
  #windows = new Map();   // `${source}:${slot}` → the copy's entry
  #metrics = null;
  #metricsAsked = -Infinity;
  #params = null;
  #paramsAsked = -Infinity;
  #generation = null;
  #wantFor;
  #now;

  /** @param {{ wantFor?: number, now?: () => number }} [options] */
  constructor({ wantFor = 1000, now = () => performance.now() } = {}) {
    this.#wantFor = wantFor;
    this.#now = now;
  }

  /**
   * The newest window of a slot, and a want for the next copy to hold it.
   * Null until a copy that holds it has been taken, and for a slot with
   * nothing to show.
   */
  window(source, slot, frames) {
    const key = `${source}:${slot}`;
    const now = this.#now();
    const want = this.#wants.get(key);
    // the most any reader of the slot asked for lately: a smaller ask is cut from it
    if (!want || now - want.asked > this.#wantFor) this.#wants.set(key, { source, slot, frames, asked: now });
    else { want.frames = Math.max(want.frames, frames); want.asked = now; }
    return newest(this.#windows.get(key)?.window ?? null, frames);
  }

  /** A slot's count of live spells, from the newest copy that held it; null before one has. A want, as window(). */
  activations(source, slot) {
    this.window(source, slot, 0);
    return this.#windows.get(`${source}:${slot}`)?.activations ?? null;
  }

  /** The slots of `source` live in the newest copy, of those asked for. */
  live(source) {
    const out = [];
    for (const entry of this.#windows.values()) if (entry.source === source && entry.window) out.push({ index: entry.slot, channels: entry.window.channels });
    return out;
  }

  /** The newest metrics part (metrics_reader.js updateSnapshot), and a want for the next copy to hold it. */
  metrics() {
    this.#metricsAsked = this.#now();
    return this.#metrics;
  }

  /** The newest parameter table (params.js readParams), and a want for the next copy to hold it. */
  params() {
    this.#paramsAsked = this.#now();
    return this.#params;
  }

  /** The wants still current, for the next copy, and whether it should hold the metrics and the parameter table. */
  wants() {
    const now = this.#now();
    const out = [];
    for (const [key, want] of this.#wants) {
      if (now - want.asked > this.#wantFor) this.#wants.delete(key);
      else out.push(want);
    }
    return { wants: out, metrics: now - this.#metricsAsked <= this.#wantFor, params: now - this.#paramsAsked <= this.#wantFor };
  }

  /** The arena generation the newest copy carried, or null before one. */
  get generation() { return this.#generation; }

  /** Take a copy (readCopy's): its windows replace those of the same slot, and its metrics the last. */
  take(copy) {
    this.#generation = copy.generation;
    if (copy.metrics) this.#metrics = copy.metrics;
    if (copy.params) this.#params = copy.params;
    for (const entry of copy.windows) this.#windows.set(`${entry.source}:${entry.slot}`, entry);
    // a slot no longer asked for is no longer held: what was last copied of it is not the slot now
    for (const key of this.#windows.keys()) if (!this.#wants.has(key)) this.#windows.delete(key);
  }
}
