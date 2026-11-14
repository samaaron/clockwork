// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Copies of the engine's memory (clockwork_client_copy, js/lib/copy.js), for
// a page that cannot read it in place.
//
//   - a scope slot is read by the tap's rule (scope_slot.js, tap_slot.js
//     ringWindow): the newest frames, clear of the oldest edge, zero-filled
//     at the front; nothing for a slot that is off or unwritten
//   - the C copy of a booted engine holds what JavaScript reads in place: the
//     arena's tables (the same constants), the metrics, the guest window and
//     the metrics history byte for byte, and each window wanted — a tap's
//     and the device scope's — sample for sample, whether its ring has
//     wrapped or not; a slot with nothing to show, or none, is a window of
//     no channels; a buffer too small is refused
//   - a Clockwork whose engine is elsewhere (mode "copies") reads its taps,
//     the device scope and its metrics from the copies its host hands it,
//     and asks for the windows it was asked for and no others
import { test } from "node:test";
import assert from "node:assert/strict";

import { readTapSlot, tapSlotViews, SLOT } from "../js/lib/tap_slot.js";
import { readScopeSlot, scopeSlotViews, SCOPE_SLOT } from "../js/lib/scope_slot.js";
import { readCopy, encodeWants, CopyReader, COPY_PART, COPY_SOURCE } from "../js/lib/copy.js";
import { readArena } from "../js/lib/arena.js";
import { readParams } from "../js/lib/params.js";
import { Clockwork } from "../js/clockwork.js";
import { bootEngine, haveWasm, testIf } from "./engine_boot.mjs";

const maybe = testIf(haveWasm, "dist/wasm/clockwork.wasm");

// A scope slot's producer: enable at `channels`, then a ramp of `count` frames from frame 0.
function writeScope(v, count, { channels = 2, capacity = v.ringFrames } = {}) {
  v.meta[SCOPE_SLOT.SAMPLE_RATE] = 48000;
  v.meta[SCOPE_SLOT.CHANNELS] = channels;
  v.meta[SCOPE_SLOT.CAPACITY] = capacity;
  for (let f = Math.max(0, count - capacity); f < count; f++) {
    const at = (f % capacity) * channels;
    for (let c = 0; c < channels; c++) v.data[at + c] = c ? -f : f;
  }
  Atomics.store(v.cursor, 0, BigInt(count));
  Atomics.store(v.meta, SCOPE_SLOT.ENABLED, 1);
}

// A tap slot's producer, as tap_slot.test.mjs's: claim, then a ramp of `count` frames.
function writeTap(v, count, channels = 2) {
  Atomics.store(v.meta, SLOT.CHANNELS, channels);
  Atomics.store(v.meta, SLOT.CAPACITY, v.ringFrames);
  Atomics.add(v.meta, SLOT.ACTIVATIONS, 1);
  for (let f = Math.max(0, count - v.ringFrames); f < count; f++) {
    const at = (f % v.ringFrames) * channels;
    for (let c = 0; c < channels; c++) v.data[at + c] = c ? -f - 0.5 : f + 0.5;
  }
  Atomics.store(v.cursor, 0, BigInt(count));
  Atomics.store(v.meta, SLOT.STATE, 1);
}

test("a scope slot is read by the tap's rule", () => {
  const FRAMES = 64, HEADER = 32;
  const make = () => scopeSlotViews(new ArrayBuffer(8 + HEADER + FRAMES * 2 * 4), 8, { headerBytes: HEADER, frames: FRAMES, channels: 2 });

  const off = make();
  assert.equal(readScopeSlot(off, 16), null, "a slot that is off has nothing");
  writeScope(off, 0);
  assert.equal(readScopeSlot(off, 16), null, "nor one with nothing written");

  const young = make();
  writeScope(young, 10);
  const w = readScopeSlot(young, 16);
  assert.equal(w.frames, 16);
  assert.equal(w.channels, 2);
  assert.equal(w.writePosition, 10n);
  assert.deepEqual([...w.interleaved.slice(0, 12)], new Array(12).fill(0), "zero-filled where nothing was written");
  assert.deepEqual([...w.interleaved.slice(12, 16)], [0, -0, 1, -1]);
  assert.deepEqual([...w.interleaved.slice(-2)], [9, -9], "ends where the writer reached");

  const wrapped = make();
  writeScope(wrapped, 1000);
  const all = readScopeSlot(wrapped, 1000);
  assert.equal(all.frames, FRAMES, "no more than the ring");
  // the margin: a quarter of a ring this small stays clear of its oldest edge
  const margin = FRAMES / 4;
  assert.deepEqual([...all.interleaved.slice(0, margin * 2)], new Array(margin * 2).fill(0));
  assert.equal(all.interleaved[margin * 2], 1000 - FRAMES + margin);
  assert.deepEqual([...all.interleaved.slice(-2)], [999, -999]);

  const wide = make();
  writeScope(wide, 10, { channels: 3, capacity: 32 });
  assert.equal(readScopeSlot(wide, 4), null, "a header wider than the ring is not believed");
});

maybe("the C copy holds what JavaScript reads in place", async () => {
  const { x, memory, arenaBase, constants: bc, handle } = await bootEngine();
  const buffer = memory.buffer;
  assert.ok(bc.TAPS_SLOTS >= 4, "the engine has tap slots to copy");

  const tapViews = (i) => tapSlotViews(buffer, arenaBase + bc.TAPS_START + bc.TAPS_HEADER_SIZE + i * bc.TAP_SLOT_SIZE,
    { headerBytes: bc.TAP_SLOT_HEADER_SIZE, ringFrames: bc.TAP_RING_FRAMES, channels: bc.TAP_CHANNELS });
  const scopeViews = (i) => scopeSlotViews(buffer, arenaBase + bc.SCOPE_START + i * bc.SCOPE_SLOT_SIZE,
    { headerBytes: bc.SCOPE_HEADER_SIZE, frames: bc.SCOPE_FRAMES, channels: bc.SCOPE_CHANNELS });

  const young = tapViews(1), wrapped = tapViews(3), out = scopeViews(bc.SCOPE_OUT_SLOT);
  writeTap(young, 700);
  writeTap(wrapped, bc.TAP_RING_FRAMES * 2 + 123);
  writeScope(out, bc.SCOPE_FRAMES + 4321);
  // the metrics carry something to see: a word in each part
  new Uint32Array(buffer, arenaBase + bc.METRICS_START, 1)[0] = 0xC0FFEE;
  new Uint32Array(buffer, arenaBase + bc.GUEST_WINDOW_START + 4, 1)[0] = 0xBEEF;

  const wants = [
    { source: COPY_SOURCE.TAP, slot: 1, frames: 1200 },
    { source: COPY_SOURCE.TAP, slot: 3, frames: 12000 },
    { source: COPY_SOURCE.TAP, slot: 2, frames: 512 },              // a slot no one has claimed
    { source: COPY_SOURCE.SCOPE, slot: bc.SCOPE_OUT_SLOT, frames: 2048 },
    { source: COPY_SOURCE.SCOPE, slot: bc.SCOPE_IN_SLOT, frames: 64 },   // a device with no input
    { source: COPY_SOURCE.TAP, slot: 4096, frames: 8 },             // no such slot
  ];
  const encoded = encodeWants(wants);
  const wantsAt = x.malloc(encoded.byteLength);
  new Uint32Array(buffer, wantsAt, encoded.length).set(encoded);
  const parts = COPY_PART.ARENA | COPY_PART.METRICS;
  const size = x.clockwork_client_copy_bytes(handle, parts, wantsAt, wants.length);
  assert.ok(size > 0, "a copy has a size");
  const at = x.malloc(size);
  assert.equal(x.clockwork_client_copy(handle, parts, wantsAt, wants.length, at, size - 1), 0, "a buffer too small is refused");
  const written = x.clockwork_client_copy(handle, parts, wantsAt, wants.length, at, size);
  assert.ok(written > 0 && written <= size, `the copy was made (${written} of ${size})`);
  const copy = readCopy(memory.buffer.slice(at, at + written));

  assert.deepEqual(readArena(copy.arena, 0).constants, bc, "the arena part is the engine's tables");
  const inPlace = (start, n) => new Uint8Array(memory.buffer.slice(arenaBase + start, arenaBase + start + n));
  const expected = new Uint8Array(bc.METRICS_SIZE + bc.GUEST_WINDOW_SIZE + bc.METRICS_HISTORY_SIZE);
  expected.set(inPlace(bc.METRICS_START, bc.METRICS_SIZE), 0);
  expected.set(inPlace(bc.GUEST_WINDOW_START, bc.GUEST_WINDOW_SIZE), bc.METRICS_SIZE);
  if (bc.METRICS_HISTORY_SIZE) expected.set(inPlace(bc.METRICS_HISTORY_START, bc.METRICS_HISTORY_SIZE), bc.METRICS_SIZE + bc.GUEST_WINDOW_SIZE);
  assert.deepEqual(new Uint8Array(copy.metrics), expected, "the metrics part is the metrics, the window and the history");
  assert.equal(new Uint32Array(copy.metrics, 0, 1)[0], 0xC0FFEE);

  assert.equal(copy.windows.length, wants.length, "a window for each want, in order");
  copy.windows.forEach((w, i) => { assert.equal(w.source, wants[i].source); assert.equal(w.slot, wants[i].slot); });
  const same = (got, want, what) => {
    assert.equal(got.frames, want.frames, `${what}: frames`);
    assert.equal(got.channels, want.channels, `${what}: channels`);
    assert.equal(got.writePosition, want.writePosition, `${what}: where the writer reached`);
    assert.deepEqual([...got.interleaved], [...want.interleaved], `${what}: the samples`);
  };
  same(copy.windows[0].window, readTapSlot(young, 1200), "a tap that has not wrapped");
  same(copy.windows[1].window, readTapSlot(wrapped, 12000), "a tap that has wrapped");
  assert.equal(copy.windows[0].activations, 1, "a tap's live spells");
  assert.equal(copy.windows[2].window, null, "a tap no one has claimed has nothing to show");
  same(copy.windows[3].window, readScopeSlot(out, 2048), "the device scope");
  assert.equal(copy.windows[4].window, readScopeSlot(scopeViews(bc.SCOPE_IN_SLOT), 64), "the input scope as JavaScript reads it");
  assert.equal(copy.windows[5].window, null, "no such slot has nothing to show");

  const bare = x.clockwork_client_copy(handle, 0, 0, 0, at, size);
  const empty = readCopy(memory.buffer.slice(at, at + bare));
  assert.deepEqual([empty.arena, empty.metrics, empty.windows.length], [null, null, 0], "nothing asked for, nothing copied");
});

maybe("a Clockwork whose engine is elsewhere reads the copies its host hands it", async (t) => {
  const { x, memory, arenaBase, constants: bc, handle } = await bootEngine();
  const buffer = memory.buffer;
  const tap = tapSlotViews(buffer, arenaBase + bc.TAPS_START + bc.TAPS_HEADER_SIZE + 2 * bc.TAP_SLOT_SIZE,
    { headerBytes: bc.TAP_SLOT_HEADER_SIZE, ringFrames: bc.TAP_RING_FRAMES, channels: bc.TAP_CHANNELS });
  const scope = scopeSlotViews(buffer, arenaBase + bc.SCOPE_START + bc.SCOPE_OUT_SLOT * bc.SCOPE_SLOT_SIZE,
    { headerBytes: bc.SCOPE_HEADER_SIZE, frames: bc.SCOPE_FRAMES, channels: bc.SCOPE_CHANNELS });
  writeTap(tap, 5000);
  writeScope(scope, 9000);
  new Uint32Array(buffer, arenaBase + bc.METRICS_START, 1)[0] = 0xC0FFEE;   // no block has run: a word to see

  // the host: what a plugin does over its own scheme, here a call into the engine
  const asked = [];
  const host = {
    async fetch(parts, wants) {
      asked.push({ parts, wants: [...wants] });
      const wantsAt = x.clockwork_worklet_alloc(Math.max(4, wants.byteLength));
      new Uint32Array(memory.buffer, wantsAt, wants.length).set(wants);
      const count = wants.length / 3;
      const size = x.clockwork_client_copy_bytes(handle, parts, wantsAt, count);
      const at = x.clockwork_worklet_alloc(size);
      const n = x.clockwork_client_copy(handle, parts, wantsAt, count, at, size);
      const out = memory.buffer.slice(at, at + n);
      x.clockwork_worklet_free(at); x.clockwork_worklet_free(wantsAt);
      return out;
    },
  };
  const engine = new Clockwork({ mode: "copies", copies: host });
  t.after(() => engine.destroy());   // its copies stop with it, pass or fail
  await engine.init();
  assert.equal(asked[0].parts & COPY_PART.ARENA, COPY_PART.ARENA, "the first copy asks for the arena's tables");

  const until = async (read, what) => {
    for (let i = 0; i < 200; i++) {
      const got = read();
      if (got) return got;
      await new Promise((r) => setTimeout(r, 10));
    }
    assert.fail(`${what} never came`);
  };
  same(await until(() => engine.getTap(2, 1500), "the tap"), readTapSlot(tap, 1500), "a tap");
  same(await until(() => engine.getDeviceScope(bc.SCOPE_OUT_SLOT, 2048), "the scope"), readScopeSlot(scope, 2048), "the device scope");
  assert.equal(await until(() => engine.getTapActivations(2), "the activations"), 1);
  const metrics = await until(() => engine.getMetricsArray()?.[0] === 0xC0FFEE ? engine.getMetricsArray() : null, "the metrics");
  assert.equal(metrics[0], 0xC0FFEE, "the metrics are the engine's");
  const last = asked.at(-1);
  assert.deepEqual(new Set(chunks(last.wants).map((w) => w.join(":"))),
    new Set([`${COPY_SOURCE.TAP}:2:1500`, `${COPY_SOURCE.SCOPE}:${bc.SCOPE_OUT_SLOT}:2048`]),
    "it asks for what was read, at the most that was asked for");
  // The parameter table (docs/PARAMS.md), for a panel in such a page: asked for once it is read, and the engine's own bytes.
  assert.ok(!asked.some((a) => a.parts & COPY_PART.PARAMS), "no table copied before one is read");
  const table = await until(() => engine.params(), "the parameter table");
  const inPlace = readParams(buffer, arenaBase + bc.PARAMS_START, 8, bc.PARAMS_SIZE);
  assert.deepEqual([table.capacity, table.generation, table.torn], [inPlace.capacity, inPlace.generation, false], "the table is the engine's");
  assert.ok(asked.some((a) => a.parts & COPY_PART.PARAMS), "the table was asked for");
  assert.throws(() => engine.send("/status"), /copies mode/, "it sends nothing: the engine is its host's");
  await assert.rejects(engine.enableMidi(), /copies mode/);

  function same(got, want, what) {
    assert.equal(got.frames, want.frames, `${what}: frames`);
    assert.equal(got.writePosition, want.writePosition, `${what}: where the writer reached`);
    assert.deepEqual([...got.interleaved], [...want.interleaved], `${what}: the samples`);
  }
  function chunks(words) {
    const out = [];
    for (let i = 0; i < words.length; i += 3) out.push(words.slice(i, i + 3));
    return out;
  }
});

test("a copy's parameter table is read, and asked for only while it is read", () => {
  // header, then a table part of 24 bytes (to the next 8), no windows
  const table = Uint8Array.from({ length: 21 }, (_, i) => i + 1);
  const buffer = new ArrayBuffer(32 + 24);
  new Uint32Array(buffer, 0, 8).set([0x59504F43, 1, 32 + 24, 7, 0, 0, 0, table.length]);
  new Uint8Array(buffer, 32, table.length).set(table);
  const copy = readCopy(buffer);
  assert.deepEqual([...new Uint8Array(copy.params)], [...table], "the table's bytes, and no more");

  let now = 0;
  const reader = new CopyReader({ wantFor: 1000, now: () => now });
  assert.equal(reader.wants().params, false, "no table until it is read");
  assert.equal(reader.params(), null, "nothing before a copy");
  assert.equal(reader.wants().params, true);
  reader.take(copy);
  assert.deepEqual([...new Uint8Array(reader.params())], [...table]);
  now = 1500;
  assert.equal(reader.wants().params, false, "a table not read for a while is let go");
  assert.equal(COPY_PART.PARAMS, 4);
});

test("a reader asks for what it read lately and no more", () => {
  let now = 0;
  const reader = new CopyReader({ wantFor: 1000, now: () => now });
  assert.equal(reader.window(COPY_SOURCE.TAP, 3, 1200), null, "nothing before a copy");
  reader.window(COPY_SOURCE.TAP, 3, 12000);
  reader.window(COPY_SOURCE.SCOPE, 0, 2048);
  assert.deepEqual(reader.wants().wants.map(({ source, slot, frames }) => [source, slot, frames]),
    [[COPY_SOURCE.TAP, 3, 12000], [COPY_SOURCE.SCOPE, 0, 2048]], "the most asked of each slot");
  assert.equal(reader.wants().metrics, false, "no metrics until they are read");
  reader.metrics();
  assert.equal(reader.wants().metrics, true);
  now = 1500;
  reader.window(COPY_SOURCE.SCOPE, 0, 2048);
  assert.deepEqual(reader.wants().wants.map(({ slot }) => slot), [0], "a slot not read for a while is let go");
  assert.equal(reader.wants().metrics, false);
});
