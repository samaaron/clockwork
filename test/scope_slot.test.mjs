// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// js/lib/scope_slot.js against a slot built by hand, laid out as
// shm_scope_stream.hpp lays it out. The reader's rules are the C++ reader's:
// a window ends at the write edge, stays clear of the ring's oldest edge by
// the read margin, and is zero-filled at the front where the ring holds
// nothing; a free slot reads as nothing; and the slot counts its live spells.
import { test } from "node:test";
import assert from "node:assert/strict";
import { SLOT, scopeSlotViews, scopeSlotLive, scopeSlotActivations, readScopeSlot } from "../js/lib/scope_slot.js";

const HEADER_BYTES = 32, RING_FRAMES = 64, CHANNELS = 2;

function slot() {
  const sab = new SharedArrayBuffer(16 + HEADER_BYTES + RING_FRAMES * CHANNELS * 4);
  const views = scopeSlotViews(sab, 16, { headerBytes: HEADER_BYTES, ringFrames: RING_FRAMES, channels: CHANNELS });
  return views;
}

// The slot's producer: claim, then write `count` frames of a ramp from frame 0.
function claimAndWrite(v, count) {
  Atomics.store(v.meta, SLOT.CHANNELS, CHANNELS);
  Atomics.store(v.meta, SLOT.CAPACITY, RING_FRAMES);
  Atomics.add(v.meta, SLOT.ACTIVATIONS, 1);
  Atomics.store(v.meta, SLOT.STATE, 1);
  for (let f = 0; f < count; f++) {
    const at = (f % RING_FRAMES) * CHANNELS;
    v.data[at] = f;
    v.data[at + 1] = -f;
  }
  Atomics.store(v.cursor, 0, BigInt(count));
}

test("the header is the C++ slot's: four u32, then two u64", () => {
  assert.deepEqual({ ...SLOT }, { STATE: 0, CHANNELS: 1, CAPACITY: 2, ACTIVATIONS: 3 });
  const v = slot();
  assert.equal(v.meta.byteOffset, 16);
  assert.equal(v.cursor.byteOffset, 16 + 16);
  assert.equal(v.data.byteOffset, 16 + HEADER_BYTES);
});

test("a free slot reads as nothing", () => {
  const v = slot();
  assert.equal(scopeSlotLive(v), false);
  assert.equal(readScopeSlot(v, 16), null);
  assert.equal(scopeSlotActivations(v), 0);
});

test("a window ends at the write edge, in order across the wrap", () => {
  const v = slot();
  claimAndWrite(v, 80);   // past the 64-frame ring: frames 64..79 sit at the ring's start
  const got = readScopeSlot(v, 16);
  assert.equal(got.frames, 16);
  assert.equal(got.channels, 2);
  assert.equal(got.writePosition, 80n);
  for (let f = 0; f < 16; f++) {
    assert.equal(got.interleaved[f * 2], 64 + f, `frame ${f} left`);
    assert.equal(got.interleaved[f * 2 + 1], -(64 + f), `frame ${f} right`);
  }
});

test("a window reaching past what the ring still holds is silence at the front", () => {
  const v = slot();
  claimAndWrite(v, 80);
  // The ring holds frames 16..79; the reader stays a quarter of a small ring
  // (16 frames) clear of its oldest edge, so frames from 32 come back.
  const got = readScopeSlot(v, 64);
  assert.equal(got.frames, 64);
  for (let f = 0; f < 16; f++) assert.equal(got.interleaved[f * 2], 0, `frame ${f} is fill`);
  assert.equal(got.interleaved[16 * 2], 32);
  assert.equal(got.interleaved[63 * 2], 79);
});

test("a slot counts its live spells, and a release does not wipe the count", () => {
  const v = slot();
  claimAndWrite(v, 8);
  assert.equal(scopeSlotActivations(v), 1);
  claimAndWrite(v, 8);   // a re-run claims it again
  assert.equal(scopeSlotActivations(v), 2);
  Atomics.store(v.meta, SLOT.STATE, 0);   // released
  assert.equal(readScopeSlot(v, 8), null);
  assert.equal(scopeSlotActivations(v), 2);
});
