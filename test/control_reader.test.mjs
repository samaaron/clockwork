// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// controlReader: the control plane's cursors as a client reads them, over a
// buffer laid out as ControlPointers (control_offsets.js).
import { test } from "node:test";
import assert from "node:assert/strict";
import { controlReader } from "../js/lib/control_reader.js";
import * as CTL from "../js/lib/control_offsets.js";

test("every cursor is read from its own word, and the status flags unsigned", () => {
  const sab = new SharedArrayBuffer(128), i32 = new Int32Array(sab, 64, 14);
  i32[CTL.IN_HEAD >> 2] = 1; i32[CTL.IN_TAIL >> 2] = 2; i32[CTL.OUT_HEAD >> 2] = 3; i32[CTL.OUT_TAIL >> 2] = 4;
  i32[CTL.NRT_OUT_HEAD >> 2] = 5; i32[CTL.NRT_OUT_TAIL >> 2] = 6; i32[CTL.IN_SEQUENCE >> 2] = 7; i32[CTL.OUT_SEQUENCE >> 2] = 8;
  i32[CTL.NRT_OUT_SEQUENCE >> 2] = 9; i32[CTL.STATUS_FLAGS >> 2] = -1;
  const read = controlReader(sab, 64), c = read();
  assert.deepEqual(c, { inHead: 1, inTail: 2, outHead: 3, outTail: 4, nrtOutHead: 5, nrtOutTail: 6, inSequence: 7, outSequence: 8, nrtOutSequence: 9, statusFlags: 0xffffffff });
  i32[CTL.IN_HEAD >> 2] = 11;
  assert.equal(read(c).inHead, 11, "a reread sees the new word, into the same object");
});
