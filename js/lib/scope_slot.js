// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// A scope slot, read from JavaScript: the device's own two rings, what left
// for it (SCOPE_OUT_SLOT) and what arrived from it (SCOPE_IN_SLOT), as
// shm_audio_buffer.hpp lays them out. The header is four u32 — enabled,
// sample rate, channels, capacity — then the write position (u64) and
// padding, then the interleaved float ring. A window of it is read by the
// rule every ring of frames is read by (tap_slot.js ringWindow), which is
// clockwork_client_copy's too.
import { ringWindow } from "./tap_slot.js";

// The header's u32 words, by index.
export const SCOPE_SLOT = Object.freeze({ ENABLED: 0, SAMPLE_RATE: 1, CHANNELS: 2, CAPACITY: 3 });

/**
 * Views of the slot at byte `offset` of `buffer`.
 * @param {ArrayBufferLike} buffer
 * @param {number} offset
 * @param {{ headerBytes: number, frames: number, channels: number }} shape  the arena's scope geometry
 */
export function scopeSlotViews(buffer, offset, { headerBytes, frames, channels }) {
  return {
    meta: new Uint32Array(buffer, offset, 4),
    cursor: new BigUint64Array(buffer, offset + 16, 1),   // write_position
    data: new Float32Array(buffer, offset + headerBytes, frames * channels),
    ringFrames: frames,
    maxChannels: channels,
  };
}

/**
 * The newest `frames` frames of the slot (at most the ring's length); null
 * for a slot the device does not have, or that nothing has been written to.
 * @returns {{ frames: number, channels: number, writePosition: bigint, interleaved: Float32Array }|null}
 */
export function readScopeSlot(views, frames) {
  if (Atomics.load(views.meta, SCOPE_SLOT.ENABLED) !== 1) return null;
  // The writer's header, possibly another process's: what it says is held to
  // the ring the engine published, as clockwork_client_scope_read holds it.
  const channels = views.meta[SCOPE_SLOT.CHANNELS];
  if (channels === 0 || channels > views.maxChannels) return null;
  let cap = views.meta[SCOPE_SLOT.CAPACITY];
  if (cap === 0 || cap > views.ringFrames) cap = views.ringFrames;
  const end = Atomics.load(views.cursor, 0);
  if (end === 0n) return null;
  return ringWindow(views.data, channels, cap, end, frames);
}
