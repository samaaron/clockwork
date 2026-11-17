// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// ringMaxMessage in js/lib/ring.js restates ring_max_message in src/ring/ring.h. Frames never wrap round the end of a
// ring, so the largest message an empty ring always takes is half of it less a header: wherever the writer is, one of
// the room before the end and the room at the front is at least half. This holds the JS writer to that rule, at every
// writer position, as test_client_abi.cpp holds the C send.
import { test } from "node:test";
import assert from "node:assert/strict";
import { ringAt, ringWrite, ringMaxMessage, HEADER_BYTES } from "../js/lib/ring.js";

const SIZE = 1024;

// An empty ring whose writer and reader are both at `at`.
function emptyRingAt(at) {
  const words = new Int32Array(4);
  const ring = ringAt(new ArrayBuffer(SIZE), 0, SIZE, words, { head: 0, tail: 1, sequence: 2, lock: 3 });
  words[ring.head] = at;
  words[ring.tail] = at;
  return ring;
}

test("the largest message is half the ring less a frame's header", () => {
  assert.equal(ringMaxMessage(SIZE), SIZE / 2 - HEADER_BYTES);
});

test("an empty ring takes the largest message wherever its writer is", () => {
  const largest = new Uint8Array(ringMaxMessage(SIZE));
  for (let at = 0; at < SIZE; at += 4) {
    assert.equal(ringWrite(emptyRingAt(at), largest), true, `writer at ${at}`);
  }
});

test("with the writer halfway round, an empty ring never takes four bytes more", () => {
  const over = new Uint8Array(ringMaxMessage(SIZE) + 4);
  assert.equal(ringWrite(emptyRingAt(SIZE / 2), over), false);
});
