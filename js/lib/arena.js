// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/**
 * arena.js — the arena's table of contents, as JavaScript reads it.
 *
 * Mirrors src/clockwork_arena.h: the header at the front of the arena, its
 * entries, the region ids and what each entry's geometry words mean. Read
 * BY ID, never by position — a reader that walked the table positionally was
 * the drift this replaces (BufferLayout, read as uint32View[n]).
 *
 * `readArena(buffer, base)` returns the header, a `region(id)` lookup, and
 * `constants` — the flat object the rest of the JS has always consumed
 * (IN_BUFFER_START, METRICS_SIZE, SHM_TAP_MAX_TAPS, …), now derived from
 * the table rather than declared beside it.
 */

export const ARENA_MAGIC = 0x43574152;      // 'CWAR'
export const ARENA_VERSION = 1;
export const ARENA_PUBLISHED = 1;
const ENTRY_WORDS = 16;                     // sizeof(ClockworkArenaEntry) / 4
const HEADER_WORDS = 16;                    // the fixed fields before the entries

export const OWNER = Object.freeze({ CLOCKWORK: 1, GUEST: 2, CLIENT: 3, HOST: 4 });

// Who READS a region: a bit set in the last geometry word of every entry
// (GEOM.AUDIENCE). Owner says who writes; this says who it is for. 0 is
// nobody but the owner, or a writer from before the word existed.
export const AUDIENCE = Object.freeze({ PUBLISHED: 1, GUEST: 2, HOST: 4, TRANSPORT: 8 });

export const REGION = Object.freeze({
  CONTROL: 1, METRICS: 2, NATIVE_STATS: 3, CLOCK_STATE: 4, CLOCK_ANCHORS: 5,
  SAMPLE_CLOCK: 6, CHANNEL_MAP: 7, NODE_ID_COUNTER: 8, IN_RING: 9, OUT_RING: 10,
  NRT_OUT_RING: 11, RAW_SCOPE: 12, GUEST_CONFIG: 14,
  GUEST_WINDOW: 15, TAPS: 16, GUEST_PERSIST: 17, TRACK_TAPS: 18,
  // 13 was the client slots (docs/THREADS.md): retired, not reused.
});

// Geometry word indices, per region (ClockworkArenaGeom).
export const GEOM = Object.freeze({
  RING_MAX_MESSAGE: 0, RING_MESSAGE_MAGIC: 1, RING_PADDING_MAGIC: 2, RING_PADDING_MARKER: 3, RING_HEADER_BYTES: 4,
  METRICS_FIELDS: 0,
  ANCHOR_NTP_START: 0, ANCHOR_DRIFT: 1, ANCHOR_GLOBAL: 2,
  RAW_SCOPE_SLOTS: 0, RAW_SCOPE_SLOT_BYTES: 1, RAW_SCOPE_HEADER_BYTES: 2, RAW_SCOPE_FRAMES: 3, RAW_SCOPE_CHANNELS: 4, RAW_SCOPE_SAMPLE_RATE: 5,
  TAP_SLOTS: 0, TAP_HEADER_BYTES: 1, TAP_SLOT_BYTES: 2, TAP_SLOT_HEADER: 3, TAP_RING_FRAMES: 4, TAP_CHANNELS: 5,
  TRACK_SLOTS: 0, TRACK_SLOT_BYTES: 1, TRACK_SLOT_HEADER: 2, TRACK_RING_FRAMES: 3, TRACK_CHANNELS: 4, TRACK_FIRST_INDEX: 5,
  // the guest window: what the guest declared its window to be, 0 until it binds
  WINDOW_MAGIC: 0, WINDOW_VERSION: 1,
  // EVERY region: the audience, in the last word (ENTRY_WORDS - 4 geometry words)
  AUDIENCE: 11,
});

/**
 * Read the header at `base` in `buffer`. Throws if it is not a published
 * arena of a version this reader knows, so a caller never proceeds on a
 * table it half-understands.
 *
 * @param {ArrayBuffer|SharedArrayBuffer} buffer
 * @param {number} base byte offset of the arena
 */
/** The regions' names, in the words the arena doc uses, by id. */
export const REGION_NAMES = Object.freeze(Object.fromEntries(Object.entries(REGION).map(([k, v]) => [v, k.toLowerCase().replace(/_/g, ' ')])));

/**
 * A region's bytes, as a view over `buffer`: what the C ABI's clockwork_client_region hands a
 * client, base and length, for a reader that wants the memory itself (a memory viewer, a dump).
 * @param {ArrayBufferLike} buffer
 * @param {number} base  the arena's base in the buffer
 * @param {{ offset: number, bytes: number }} entry  from readArena's entries
 */
export function regionBytes(buffer, base, entry) {
  const at = base + entry.offset;
  if (at + entry.bytes > buffer.byteLength) throw new Error(`region at ${at} runs past the buffer (${entry.bytes} bytes)`);
  return new Uint8Array(buffer, at, entry.bytes);
}

export function readArena(buffer, base) {
  if (base % 4 !== 0) throw new Error(`arena base ${base} is not 4-byte aligned`);
  const words = new Uint32Array(buffer, base, HEADER_WORDS);
  const [magic, version, headerBytes, instanceId, arenaBytes, blockBytes, guestOffset, guestBytes,
         entryCount, entryBytes, state, blockPublishedEnd, guestPublishedEnd] = words;
  if (magic !== ARENA_MAGIC) throw new Error(`not a clockwork arena (magic ${magic.toString(16)})`);
  if (version !== ARENA_VERSION) throw new Error(`arena version ${version}, this reader knows ${ARENA_VERSION}`);
  if (state !== ARENA_PUBLISHED) throw new Error('arena not yet published');
  if (entryBytes !== ENTRY_WORDS * 4) throw new Error(`arena entry shape ${entryBytes} bytes, expected ${ENTRY_WORDS * 4}`);

  const table = new Uint32Array(buffer, base + HEADER_WORDS * 4, entryCount * ENTRY_WORDS);
  const entries = new Map();
  for (let i = 0; i < entryCount; i++) {
    const at = i * ENTRY_WORDS;
    const id = table[at];
    if (id === 0) throw new Error(`arena entry ${i} is empty`);
    const offset = table[at + 1], bytes = table[at + 2];
    if (offset < headerBytes || offset + bytes > arenaBytes) throw new Error(`arena region ${id} lies outside the arena`);
    const geom = Array.from(table.subarray(at + 4, at + ENTRY_WORDS));
    entries.set(id, { id, offset, bytes, owner: table[at + 3], geom, audience: geom[GEOM.AUDIENCE] });
  }
  // The published runs, when the writer stated them: each inside its half,
  // every published region inside its run, everything else after it.
  if (blockPublishedEnd !== 0 || guestPublishedEnd !== 0) {
    if (blockPublishedEnd < headerBytes || blockPublishedEnd > blockBytes)
      throw new Error("the block's published run runs past the block");
    if (guestPublishedEnd < guestOffset || guestPublishedEnd > guestOffset + guestBytes)
      throw new Error("the guest's published run is outside the guest region");
    for (const e of entries.values()) {
      const end = e.offset >= guestOffset ? guestPublishedEnd : blockPublishedEnd;
      const published = (e.audience & AUDIENCE.PUBLISHED) !== 0;
      if (published && e.offset + e.bytes > end) throw new Error(`arena region ${e.id} is published but outside the published run`);
      if (!published && e.offset < end) throw new Error(`arena region ${e.id} is inside the published run but not published`);
    }
  }
  const region = (id) => {
    const e = entries.get(id);
    if (!e) throw new Error(`arena has no region ${id}`);
    return e;
  };
  // A data contract a client may read by hand: the region exists and says so.
  const published = (id) => {
    const e = entries.get(id);
    return !!e && (e.audience & AUDIENCE.PUBLISHED) !== 0;
  };

  return {
    header: { magic, version, headerBytes, instanceId, arenaBytes, blockBytes, guestOffset, guestBytes, entryCount,
              blockPublishedEnd, guestPublishedEnd },
    entries,
    region,
    published,
    has: (id) => entries.has(id),
    // The worklet's flat constants need every region the engine carves; a
    // reader of some other arena (a test's, a partial one) may never ask.
    get constants() {
      return constantsFrom(region, entries, { arenaBytes, blockBytes, guestOffset, guestBytes, version, instanceId });
    },
  };
}

/**
 * The flat constants object the JS runtime consumes, derived from the table.
 * The names are the ones shared_memory.h has always used, so a reader of
 * either side recognises them; the values come from THIS engine's table.
 */
function constantsFrom(region, entries, h) {
  // Optional: an arena with no track taps has no entry, which is zero slots.
  const track = entries.get(REGION.TRACK_TAPS) || { offset: 0, bytes: 0, geom: [] };
  const inRing = region(REGION.IN_RING), outRing = region(REGION.OUT_RING), nrt = region(REGION.NRT_OUT_RING);
  const control = region(REGION.CONTROL), metrics = region(REGION.METRICS), window = region(REGION.GUEST_WINDOW);
  const anchors = region(REGION.CLOCK_ANCHORS), clock = region(REGION.CLOCK_STATE), raw = region(REGION.RAW_SCOPE);
  const nodeId = region(REGION.NODE_ID_COUNTER), config = region(REGION.GUEST_CONFIG), taps = region(REGION.TAPS);
  const sample = region(REGION.SAMPLE_CLOCK), persist = region(REGION.GUEST_PERSIST), map = region(REGION.CHANNEL_MAP);
  return {
    ARENA_VERSION: h.version,
    ARENA_INSTANCE_ID: h.instanceId,
    CLOCKWORK_BLOCK_SIZE: h.blockBytes,
    GUEST_REGION_START: h.guestOffset,
    GUEST_REGION_SIZE: h.guestBytes,
    TOTAL_BUFFER_SIZE: h.arenaBytes,

    IN_BUFFER_START: inRing.offset, IN_BUFFER_SIZE: inRing.bytes,
    OUT_BUFFER_START: outRing.offset, OUT_BUFFER_SIZE: outRing.bytes,
    NRT_OUT_BUFFER_START: nrt.offset, NRT_OUT_BUFFER_SIZE: nrt.bytes,
    MAX_MESSAGE_SIZE: inRing.geom[GEOM.RING_MAX_MESSAGE],
    MESSAGE_MAGIC: inRing.geom[GEOM.RING_MESSAGE_MAGIC],
    PADDING_MAGIC: inRing.geom[GEOM.RING_PADDING_MAGIC],
    RING_PADDING_MARKER: inRing.geom[GEOM.RING_PADDING_MARKER],
    MESSAGE_HEADER_SIZE: inRing.geom[GEOM.RING_HEADER_BYTES],

    CONTROL_START: control.offset, CONTROL_SIZE: control.bytes,
    METRICS_START: metrics.offset, METRICS_SIZE: metrics.geom[GEOM.METRICS_FIELDS] * 4,
    SHM_WINDOW_START: window.offset, SHM_WINDOW_SIZE: window.bytes,
    NTP_START_TIME_START: anchors.offset + anchors.geom[GEOM.ANCHOR_NTP_START], NTP_START_TIME_SIZE: 8,
    DRIFT_OFFSET_START: anchors.offset + anchors.geom[GEOM.ANCHOR_DRIFT], DRIFT_OFFSET_SIZE: 4,
    GLOBAL_OFFSET_START: anchors.offset + anchors.geom[GEOM.ANCHOR_GLOBAL], GLOBAL_OFFSET_SIZE: 4,
    CLOCK_STATE_START: clock.offset, CLOCK_STATE_SIZE: clock.bytes,
    SHM_AUDIO_START: raw.offset, SHM_AUDIO_TOTAL_SIZE: raw.bytes,
    SHM_AUDIO_SLOTS: raw.geom[GEOM.RAW_SCOPE_SLOTS],
    SHM_AUDIO_OUT_SLOT: 0,   // CLOCKWORK_TAP_OUT: what left for the device
    SHM_AUDIO_IN_SLOT: 1,    // CLOCKWORK_TAP_IN: what arrived from it
    SHM_AUDIO_SLOT_SIZE: raw.geom[GEOM.RAW_SCOPE_SLOT_BYTES],
    SHM_AUDIO_HEADER_SIZE: raw.geom[GEOM.RAW_SCOPE_HEADER_BYTES],
    SHM_AUDIO_FRAMES: raw.geom[GEOM.RAW_SCOPE_FRAMES],
    SHM_AUDIO_CHANNELS: raw.geom[GEOM.RAW_SCOPE_CHANNELS],
    SHM_AUDIO_SAMPLE_RATE: raw.geom[GEOM.RAW_SCOPE_SAMPLE_RATE],
    NODE_ID_COUNTER_START: nodeId.offset, NODE_ID_COUNTER_SIZE: nodeId.bytes,
    GUEST_CONFIG_START: config.offset, GUEST_CONFIG_SIZE: config.bytes,
    SHM_TAP_START: taps.offset, SHM_TAP_TOTAL_SIZE: taps.bytes,
    SHM_TAP_MAX_TAPS: taps.geom[GEOM.TAP_SLOTS],
    SHM_TAP_HEADER_SIZE: taps.geom[GEOM.TAP_HEADER_BYTES],
    SHM_TAP_SLOT_SIZE: taps.geom[GEOM.TAP_SLOT_BYTES],
    SHM_TAP_SLOT_HEADER_SIZE: taps.geom[GEOM.TAP_SLOT_HEADER],
    SHM_TAP_RING_FRAMES: taps.geom[GEOM.TAP_RING_FRAMES],
    SHM_TAP_CHANNELS: taps.geom[GEOM.TAP_CHANNELS],
    // The engine's track taps: tap slots numbered after the guest's.
    SHM_TRACK_TAPS_START: track.offset,
    SHM_TRACK_TAPS_SIZE: track.bytes,
    SHM_TRACK_TAPS_SLOTS: track.geom[GEOM.TRACK_SLOTS] || 0,
    SHM_TRACK_TAPS_FIRST_INDEX: track.geom[GEOM.TRACK_FIRST_INDEX] || taps.geom[GEOM.TAP_SLOTS],
    // Every slot a client can number: the guest's, then the track taps.
    SHM_TAP_SLOT_COUNT: taps.geom[GEOM.TAP_SLOTS] + (track.geom[GEOM.TRACK_SLOTS] || 0),
    SAMPLE_CLOCK_START: sample.offset, SAMPLE_CLOCK_SIZE: sample.bytes,
    GUEST_PERSIST_START: persist.offset, GUEST_PERSIST_SIZE: persist.bytes,
    CHANNEL_MAP_START: map.offset, CHANNEL_MAP_SIZE: map.bytes,
  };
}
