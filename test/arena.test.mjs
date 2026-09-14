// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// js/lib/arena.js against a table built by hand, the shape writeArenaHeader
// writes: the audience word, the published runs, and the refusals. No wasm:
// the reader's rules are the C header's, and wasm_client.test.mjs holds the
// reader to a real engine's table.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readArena, AUDIENCE, GEOM, REGION, ARENA_MAGIC, ARENA_VERSION, ARENA_PUBLISHED, OWNER } from "../js/lib/arena.js";

const HEADER_WORDS = 16, ENTRY_WORDS = 16, HEADER_BYTES = 4096;

function table({ blockEnd = 6144, guestEnd = 10240, entries } = {}) {
  const buf = new ArrayBuffer(12288);
  const w = new Uint32Array(buf, 0, HEADER_WORDS + 48 * ENTRY_WORDS);
  const rows = entries ?? [
    [REGION.METRICS,       4096, 1024, OWNER.CLOCKWORK, AUDIENCE.PUBLISHED],
    [REGION.CLOCK_STATE,   5120, 1024, OWNER.CLOCKWORK, AUDIENCE.PUBLISHED | AUDIENCE.GUEST],
    [REGION.IN_RING,       6144, 2048, OWNER.CLIENT,    AUDIENCE.TRANSPORT],
    [REGION.GUEST_WINDOW,  8192, 2048, OWNER.GUEST,     AUDIENCE.PUBLISHED],
    [REGION.GUEST_PERSIST, 10240, 2048, OWNER.GUEST,    AUDIENCE.GUEST],
  ];
  w.set([ARENA_MAGIC, ARENA_VERSION, HEADER_BYTES, 0, 12288, 8192, 8192, 4096, rows.length, ENTRY_WORDS * 4,
         ARENA_PUBLISHED, blockEnd, guestEnd]);
  rows.forEach(([id, offset, bytes, owner, audience], i) => {
    const at = HEADER_WORDS + i * ENTRY_WORDS;
    w[at] = id; w[at + 1] = offset; w[at + 2] = bytes; w[at + 3] = owner;
    w[at + 4 + GEOM.AUDIENCE] = audience;
  });
  return { buf, w };
}

test("the audience word is the last geometry word, and published() reads it", () => {
  assert.equal(GEOM.AUDIENCE, ENTRY_WORDS - 4 - 1);
  const a = readArena(table().buf, 0);
  assert.equal(a.header.blockPublishedEnd, 6144);
  assert.equal(a.header.guestPublishedEnd, 10240);
  assert.equal(a.region(REGION.METRICS).audience, AUDIENCE.PUBLISHED);
  assert.ok(a.published(REGION.METRICS));
  assert.ok(a.published(REGION.GUEST_WINDOW));
  assert.ok(!a.published(REGION.IN_RING));
  assert.ok(!a.published(REGION.GUEST_PERSIST));
  assert.ok(!a.published(REGION.SCOPE), "an absent region is not published");
  assert.equal(a.region(REGION.CLOCK_STATE).audience & AUDIENCE.GUEST, AUDIENCE.GUEST);
});

test("a published region outside the published run is refused", () => {
  const { buf, w } = table();
  w[HEADER_WORDS + 2 * ENTRY_WORDS + 4 + GEOM.AUDIENCE] = AUDIENCE.PUBLISHED;   // IN_RING
  assert.throws(() => readArena(buf, 0), /published but outside the published run/);
});

test("an unpublished region inside the published run is refused", () => {
  const { buf, w } = table();
  w[HEADER_WORDS + 0 * ENTRY_WORDS + 4 + GEOM.AUDIENCE] = AUDIENCE.TRANSPORT;   // METRICS
  assert.throws(() => readArena(buf, 0), /inside the published run but not published/);
});

test("a boundary outside its half is refused", () => {
  assert.throws(() => readArena(table({ blockEnd: 8192 + 16 }).buf, 0), /published run runs past the block/);
  assert.throws(() => readArena(table({ guestEnd: 8192 - 16 }).buf, 0), /outside the guest region/);
});

test("a writer that said nothing about runs is read, and publishes nothing", () => {
  const { buf, w } = table({ blockEnd: 0, guestEnd: 0 });
  for (let i = 0; i < 5; i++) w[HEADER_WORDS + i * ENTRY_WORDS + 4 + GEOM.AUDIENCE] = 0;
  const a = readArena(buf, 0);
  assert.equal(a.header.blockPublishedEnd, 0);
  assert.ok(!a.published(REGION.METRICS));
});
