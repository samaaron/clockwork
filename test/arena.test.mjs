// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// js/lib/arena.js against a table built by hand, the shape writeArenaHeader
// writes: the areas, the five facts of each region, and the refusals. No
// wasm: the reader's rules are the C header's, and engine_boot.mjs holds the
// reader to a real engine's table.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readArena, regionBytes, readGeneration, instanceGeneration, hasInstance,
         AREA, SPACE, AUDIENCE, CADENCE, TIER, LIFETIME, REGION,
         ARENA_MAGIC, ARENA_VERSION, ARENA_PUBLISHED, OWNER } from "../js/lib/arena.js";

const HEADER_WORDS = 16, AREA_WORDS = 6, MAX_AREAS = 16, ENTRY_WORDS = 16, HEADER_BYTES = 4096;
const ENTRIES_AT = HEADER_WORDS + MAX_AREAS * AREA_WORDS;

// A small arena: a state area of two regions, a transport area of one; a
// guest half with a public window and a private store.
function table({ areas, entries } = {}) {
  const buf = new ArrayBuffer(12288);
  const w = new Uint32Array(buf, 0, ENTRIES_AT + 48 * ENTRY_WORDS);
  const as = areas ?? [
    [AREA.CLOCKWORK_STATE,     SPACE.ARENA, 4096,  2048],
    [AREA.CLOCKWORK_TRANSPORT, SPACE.ARENA, 6144,  2048],
    [AREA.GUEST_PUBLIC,        SPACE.ARENA, 8192,  2048],
    [AREA.GUEST_PRIVATE,       SPACE.ARENA, 10240, 2048],
    [AREA.GUEST_MEMORY_BULK,   SPACE.UNKNOWN, 0, 0],
  ];
  const rows = entries ?? [
    [REGION.CLOCKWORK_METRICS,     4096,  1024, OWNER.CLOCKWORK, AUDIENCE.PUBLIC,                 CADENCE.BLOCK,  TIER.FAST, LIFETIME.COLD_BOOT],
    [REGION.CLOCKWORK_CLOCK_STATE, 5120,  1024, OWNER.CLOCKWORK, AUDIENCE.PUBLIC | AUDIENCE.GUEST, CADENCE.BLOCK,  TIER.FAST, LIFETIME.COLD_BOOT],
    [REGION.CLOCKWORK_IN_RING,     6144,  2048, OWNER.CLIENT,    AUDIENCE.TRANSPORT,              CADENCE.BLOCK,  TIER.FAST, LIFETIME.COLD_BOOT],
    [REGION.GUEST_WINDOW,          8192,  2048, OWNER.GUEST,     AUDIENCE.PUBLIC,                 CADENCE.CHANGE, TIER.BULK, LIFETIME.REBUILD],
    [REGION.GUEST_PERSIST,         10240, 2048, OWNER.GUEST,     AUDIENCE.GUEST,                  CADENCE.CHANGE, TIER.BULK, LIFETIME.COLD_BOOT],
  ];
  // ... state, then the generation word: 1, a new arena with no instance yet.
  w.set([ARENA_MAGIC, ARENA_VERSION, HEADER_BYTES, 0, 12288, 8192, 8192, 4096,
         as.length, AREA_WORDS * 4, rows.length, ENTRY_WORDS * 4, ARENA_PUBLISHED, 1]);
  as.forEach(([area, space, base, bytes], i) => {
    const at = HEADER_WORDS + i * AREA_WORDS;
    w[at] = area; w[at + 1] = space; w[at + 2] = base; w[at + 4] = bytes;
  });
  rows.forEach((row, i) => w.set(row, ENTRIES_AT + i * ENTRY_WORDS));
  return { buf, w, areaWord: (i, k) => HEADER_WORDS + i * AREA_WORDS + k, entryWord: (i, k) => ENTRIES_AT + i * ENTRY_WORDS + k };
}

test("the areas and the five facts are read, by id", () => {
  const a = readArena(table().buf, 0);
  assert.equal(a.header.clockworkBytes, 8192);
  assert.equal(a.header.guestOffset, 8192);
  assert.equal(a.header.areaCount, 5);
  assert.deepEqual(a.area(AREA.CLOCKWORK_TRANSPORT), { area: AREA.CLOCKWORK_TRANSPORT, space: SPACE.ARENA, base: 6144, bytes: 2048 });
  assert.equal(a.area(AREA.GUEST_MEMORY_BULK).space, SPACE.UNKNOWN, "an area the engine has not placed says so");
  assert.equal(a.area(AREA.ASSETS), null, "an area the table does not list");
  const m = a.region(REGION.CLOCKWORK_METRICS);
  assert.equal(m.audience, AUDIENCE.PUBLIC);
  assert.equal(m.cadence, CADENCE.BLOCK);
  assert.equal(m.tier, TIER.FAST);
  assert.equal(m.lifetime, LIFETIME.COLD_BOOT);
  assert.equal(m.area, AREA.CLOCKWORK_STATE);
  assert.equal(a.areaOf(a.region(REGION.GUEST_PERSIST)), AREA.GUEST_PRIVATE);
  assert.ok(a.public(REGION.CLOCKWORK_METRICS));
  assert.ok(a.public(REGION.GUEST_WINDOW));
  assert.ok(!a.public(REGION.CLOCKWORK_IN_RING));
  assert.ok(!a.public(REGION.GUEST_PERSIST));
  assert.ok(!a.public(REGION.CLOCKWORK_TAPS), "an absent region is not public");
  assert.equal(a.region(REGION.CLOCKWORK_CLOCK_STATE).audience & AUDIENCE.GUEST, AUDIENCE.GUEST);
});

test("a public region in the transport area is refused", () => {
  const { buf, w, entryWord } = table();
  w[entryWord(2, 4)] = AUDIENCE.PUBLIC;   // IN_RING
  assert.throws(() => readArena(buf, 0), /public but outside the public areas/);
});

test("a non-public region in a public area is refused", () => {
  const { buf, w, entryWord } = table();
  w[entryWord(0, 4)] = AUDIENCE.TRANSPORT;   // METRICS
  assert.throws(() => readArena(buf, 0), /in a public area but not public/);
});

test("a region in no area, and a fact left unsaid, are refused", () => {
  const { buf, w, entryWord } = table();
  w[entryWord(0, 1)] = 4096 + 1536;   // METRICS runs out of the state area
  assert.throws(() => readArena(buf, 0), /is in no area/);
  const t = table();
  t.w[t.entryWord(1, 6)] = 0;   // CLOCK_STATE's tier
  assert.throws(() => readArena(t.buf, 0), /has no tier/);
});

test("an area in the wrong half, or overlapping another, is refused", () => {
  const { buf, w, areaWord } = table();
  w[areaWord(1, 4)] = 4096;   // transport runs into the guest's half
  assert.throws(() => readArena(buf, 0), /an area in the wrong half/);
  const t = table();
  t.w[t.areaWord(1, 2)] = 5120;   // transport overlaps state
  assert.throws(() => readArena(t.buf, 0), /two areas overlap/);
});

test("the halves must meet and cover the arena", () => {
  const { buf, w } = table();
  w[6] = 8192 + 16;   // guestOffset
  assert.throws(() => readArena(buf, 0), /does not start where clockwork's ends/);
});

test("the generation word says whether there is an instance, and which one, live", () => {
  // One word, one load (docs/RESTARTS.md): odd between instances, even with
  // one; the rest is the generation the last build opened — the number
  // /clockwork/setup named. readArena snapshots it; readGeneration reads it
  // live, since it moves while a page holds the table.
  const { buf, w } = table();
  const a = readArena(buf, 0);
  assert.equal(a.header.generation, 1, "a new arena: no instance yet, none opened");
  assert.equal(hasInstance(a.header.generation), false);
  assert.equal(instanceGeneration(a.header.generation), 0);
  w[13] = 2;                                      // the first instance is up
  assert.equal(readGeneration(buf, 0), 2);
  assert.equal(hasInstance(2), true);
  assert.equal(instanceGeneration(2), 1);
  w[13] = 3;                                      // torn down; nothing built yet
  assert.equal(hasInstance(readGeneration(buf, 0)), false);
  assert.equal(instanceGeneration(readGeneration(buf, 0)), 1, "the one closing is still the one named");
  w[13] = 4;
  assert.equal(instanceGeneration(readGeneration(buf, 0)), 2);
  assert.equal(a.header.generation, 1, "the snapshot is a snapshot");
});

test("regionBytes is the region's bytes as a view, and refuses one past the buffer", () => {
  const { buf } = table();
  const a = readArena(buf, 0), e = a.region(REGION.CLOCKWORK_METRICS);
  const v = regionBytes(buf, 0, e);
  assert.equal(v.byteOffset, 4096); assert.equal(v.byteLength, 1024);
  new Uint8Array(buf)[4096] = 0x5a;
  assert.equal(v[0], 0x5a, "a view, not a copy");
  assert.throws(() => regionBytes(buf, 0, { offset: 12000, bytes: 1024 }));
});
