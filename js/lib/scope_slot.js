// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// A scope slot, read from JavaScript: the protocol shm_scope_stream.hpp's
// reader speaks, over views of the shared buffer. The slot's header is four
// u32 — state (0 free, 1 live), channels, capacity, activations (the times it
// has gone live) — then write_position and base_engine_frames (u64), then the
// interleaved float ring. See docs/PORTS.md.

// The header's u32 words, by index.
export const SLOT = Object.freeze({ STATE: 0, CHANNELS: 1, CAPACITY: 2, ACTIVATIONS: 3 });

// Frames a reader keeps clear of the ring's oldest edge: the writer appends
// several blocks back-to-back per hardware callback. SHM_SCOPE_READ_MARGIN_FRAMES,
// clamped to a quarter of a small ring as copy_window clamps it.
const READ_MARGIN_FRAMES = 2048;

/**
 * Views of the slot at byte `offset` of `buffer`.
 * @param {ArrayBufferLike} buffer
 * @param {number} offset
 * @param {{ headerBytes: number, ringFrames: number, channels: number }} shape  the arena's slot geometry
 */
export function scopeSlotViews(buffer, offset, { headerBytes, ringFrames, channels }) {
  return {
    meta: new Uint32Array(buffer, offset, 4),
    cursor: new BigUint64Array(buffer, offset + 16, 2),   // [write_position, base_engine_frames]
    data: new Float32Array(buffer, offset + headerBytes, ringFrames * channels),
    ringFrames,
    maxChannels: channels,
  };
}

/** Whether the slot is live now. */
export const scopeSlotLive = (views) => Atomics.load(views.meta, SLOT.STATE) === 1;

/**
 * The times the slot has gone live. A reader polls, and a slot can be claimed
 * and released between two polls; a reader that noted this and sees another
 * number knows the slot went live in between. Wraps: compare for change only.
 */
export const scopeSlotActivations = (views) => Atomics.load(views.meta, SLOT.ACTIVATIONS);

/**
 * The newest `frames` frames of a live slot (at most the ring's length), the
 * window ending at the write edge and zero-filled at the front where the ring
 * no longer holds it; null for a slot that is free or not yet written.
 * @returns {{ frames: number, channels: number, writePosition: bigint, interleaved: Float32Array }|null}
 */
export function readScopeSlot(views, frames) {
  if (!scopeSlotLive(views)) return null;
  // Untrusted runtime value: clamp so a corrupt slot can't index outside the
  // ring views.
  const channels = Math.min(Math.max(views.meta[SLOT.CHANNELS], 1), views.maxChannels);
  const cap = views.ringFrames;
  const end = Atomics.load(views.cursor, 0);
  if (end === 0n) return null;

  const want = Math.min(frames, cap);
  let start = end > BigInt(want) ? end - BigInt(want) : 0n;
  const margin = BigInt(Math.min(cap >> 2, READ_MARGIN_FRAMES));
  const oldest = end > BigInt(cap) ? end - BigInt(cap) + margin : 0n;
  if (start < oldest) start = oldest;

  const real = Number(end - start);
  const out = new Float32Array(want * channels);   // zero-filled lead-in
  const fill = want - real;
  let at = Number(start % BigInt(cap));
  for (let i = 0; i < real; i++) {
    for (let c = 0; c < channels; c++) out[(fill + i) * channels + c] = views.data[at * channels + c];
    at = (at + 1) % cap;
  }
  return { frames: want, channels, writePosition: end, interleaved: out };
}
