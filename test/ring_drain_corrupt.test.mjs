// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// ringDrain over a header another writer corrupted: the walk in
// js/lib/ring.js restates src/lanes/ring_drain.h, and is held to its
// corruption rules the same way (test_in_ring_drain.cpp).
import { test } from "node:test";
import assert from "node:assert/strict";
import { ringAt, ringDrain, HEADER_BYTES } from "../js/lib/ring.js";

const SIZE = 1024;

function ringWithHeader(length) {
  const words = new Int32Array(4);
  const ring = ringAt(new ArrayBuffer(SIZE), 0, SIZE, words, { head: 0, tail: 1, sequence: 2, lock: 3 });
  ring.view.setUint32(0, 0xDEADBEEF, true);
  ring.view.setUint32(4, length, true);
  words[ring.head] = HEADER_BYTES;   // one header published
  return { ring, words };
}

test("a length whose footprint rounding overflows is dropped and the tail resynced", () => {
  // (0xFFFFFFFF + 3) & ~3 is 0 in int32 arithmetic: a footprint of nothing
  // passes every fit check while the payload claimed 4 GB. Found by
  // fuzz_ring_drain against the C++ walk (2026-10-29); the same arithmetic
  // is here.
  const { ring, words } = ringWithHeader(0xFFFFFFFF);
  const state = { lastSeq: -1 };
  const delivered = [];
  const consumed = ringDrain(ring, { words, index: ring.tail }, state, 0,
    (origin, offset, length) => delivered.push({ offset, length }));
  assert.equal(consumed, 0);
  assert.deepEqual(delivered, []);
  assert.equal(state.stop, "bad-length");
  assert.equal(words[ring.tail], words[ring.head], "resynced to head");
});

test("a length past the ring is dropped and the tail resynced", () => {
  const { ring, words } = ringWithHeader(SIZE + 64);
  const state = { lastSeq: -1 };
  const consumed = ringDrain(ring, { words, index: ring.tail }, state, 0, () => {});
  assert.equal(consumed, 0);
  assert.equal(state.stop, "bad-length");
  assert.equal(words[ring.tail], words[ring.head]);
});

test("a padding marker the writer did not wrap for is corruption and not a loop", () => {
  // A header-only frame at 0, a marker at 16, head at 32: the writer never
  // wrapped, so following the marker back to 0 would deliver the same frame
  // for ever. Found by fuzz_ring_drain against the C++ walk (2026-10-29).
  const words = new Int32Array(4);
  const ring = ringAt(new ArrayBuffer(SIZE), 0, SIZE, words, { head: 0, tail: 1, sequence: 2, lock: 3 });
  ring.view.setUint32(0, 0xDEADBEEF, true);
  ring.view.setUint32(4, HEADER_BYTES, true);
  ring.view.setUint32(16, 0xBADDCAFE, true);
  words[ring.head] = 2 * HEADER_BYTES;
  const state = { lastSeq: -1 };
  const consumed = ringDrain(ring, { words, index: ring.tail }, state, 8, () => {});
  assert.ok(consumed <= 1, `the frame is taken at most once, not ${consumed} times`);
  assert.equal(state.stop, "bad-magic");
  assert.equal(words[ring.tail], words[ring.head], "resynced to head");
});
