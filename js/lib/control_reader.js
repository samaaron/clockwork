// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron

/**
 * The control plane, read where the engine and its clients keep the rings'
 * cursors (ControlPointers in src/shared_memory.h; the offsets are
 * control_offsets.js): each ring's head and tail, the sequence number of the
 * last message either way, and the status flags. What a client that watches
 * the transport reads, without an opinion about what the numbers mean.
 */

import * as CTL from "./control_offsets.js";

/**
 * A reader over the control region at `byteOffset` in `buffer`. Returns read(into = {}): `into`, filled
 * with the cursors, as plain numbers, each an atomic load. A caller that reads often passes the same
 * object each time, and nothing is allocated.
 * @param {ArrayBufferLike|(() => ArrayBufferLike)} buffer
 * @param {number} byteOffset
 */
export function controlReader(buffer, byteOffset) {
  const source = typeof buffer === "function" ? buffer : () => buffer;
  let at = null, i32 = null;
  return function read(into = {}) {
    const b = source();
    if (b !== at) { at = b; i32 = new Int32Array(b, byteOffset, 14); }
    into.inHead = Atomics.load(i32, CTL.IN_HEAD >> 2);
    into.inTail = Atomics.load(i32, CTL.IN_TAIL >> 2);
    into.outHead = Atomics.load(i32, CTL.OUT_HEAD >> 2);
    into.outTail = Atomics.load(i32, CTL.OUT_TAIL >> 2);
    into.nrtOutHead = Atomics.load(i32, CTL.NRT_OUT_HEAD >> 2);
    into.nrtOutTail = Atomics.load(i32, CTL.NRT_OUT_TAIL >> 2);
    into.inSequence = Atomics.load(i32, CTL.IN_SEQUENCE >> 2);
    into.outSequence = Atomics.load(i32, CTL.OUT_SEQUENCE >> 2);
    into.nrtOutSequence = Atomics.load(i32, CTL.NRT_OUT_SEQUENCE >> 2);
    into.statusFlags = Atomics.load(i32, CTL.STATUS_FLAGS >> 2) >>> 0;
    return into;
  };
}
