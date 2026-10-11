// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// AudioCapture.read({ frames }): the newest that many frames of a raw scope
// slot, for a reader that draws a window. Over a synthetic slot: the header
// shm_audio_buffer.hpp lays out, then the interleaved ring.
import { test } from "node:test";
import assert from "node:assert/strict";
import { AudioCapture } from "../js/lib/audio_capture.js";

const HEADER_BYTES = 32;
function slot({ capacity, channels, wpos }) {
  const floats = capacity * channels, sab = new SharedArrayBuffer(HEADER_BYTES + floats * 4);
  const h = new Uint32Array(sab, 0, HEADER_BYTES / 4);
  h[1] = 48000; h[2] = channels; h[3] = capacity; h[4] = wpos; h[5] = 0;
  const data = new Float32Array(sab, HEADER_BYTES, floats);
  for (let f = 0; f < capacity; f++) for (let c = 0; c < channels; c++) data[f * channels + c] = f * 10 + c;   // frame f, channel c, as written at absolute frame f (mod capacity)
  const cap = new AudioCapture({ sharedBuffer: sab, ringBufferBase: 0, bufferConstants: { SHM_AUDIO_START: 0, SHM_AUDIO_HEADER_SIZE: HEADER_BYTES, SHM_AUDIO_SLOT_SIZE: HEADER_BYTES + floats * 4 } });
  return cap;
}

test("read() without a window is the newest ring's worth", () => {
  const r = slot({ capacity: 8, channels: 2, wpos: 8 }).read();
  assert.equal(r.frames, 8);
  assert.equal(r.left[0], 0); assert.equal(r.right[7], 71);
});

test("read({ frames }) is the newest that many, ending at the writer, in order", () => {
  const r = slot({ capacity: 8, channels: 2, wpos: 8 }).read({ frames: 3 });
  assert.equal(r.frames, 3);
  assert.deepEqual([...r.left], [50, 60, 70]);
  assert.deepEqual([...r.right], [51, 61, 71]);
  assert.equal(r.lost, 0, "a window asked for is not frames lost");
  assert.equal(r.capacity, 8, "and the ring's length comes with it");
});

test("a window wider than the ring is the ring, and one wider than what is written is what is written", () => {
  assert.equal(slot({ capacity: 8, channels: 1, wpos: 100 }).read({ frames: 50 }).frames, 8);
  assert.equal(slot({ capacity: 8, channels: 1, wpos: 3 }).read({ frames: 50 }).frames, 3);
});

test("the window follows the ring round: frames across the wrap come out in time order", () => {
  const r = slot({ capacity: 8, channels: 1, wpos: 10 }).read({ frames: 4 });   // absolute frames 6..9; 8 and 9 sit at ring 0 and 1
  assert.deepEqual([...r.left], [60, 70, 0, 10]);
});
