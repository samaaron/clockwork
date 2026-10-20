// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The browser's writer of the in ring at the boundary docs/RESTARTS.md draws:
// while the engine has no guest instance, a message for the guest is refused
// where it is written, with the word every boundary uses, and clockwork's own
// goes through. The word is the engine's (the arena's generation word, odd
// from a teardown until the next build); here it is turned odd by hand over a
// real engine booted in node, since the web build has no device to swap.
import { test } from "node:test";
import assert from "node:assert/strict";
import { OscChannel, REASON_RING_FULL } from "../js/lib/osc_channel.js";
import { REASON_NO_INSTANCE, oscAddress, isClockworkSysBytes } from "../js/lib/clockwork_sys.js";
import { readArena, readGeneration, generationView, instanceGeneration, hasInstance } from "../js/lib/arena.js";
import * as oscFast from "../js/lib/osc_fast.js";
import { EgressReader } from "../js/lib/ring.js";
import { bootEngine, block, haveWasm } from "./engine_boot.mjs";
const maybe = haveWasm ? test : test.skip;

maybe("a booted engine's arena names its first instance: generation 1, and one is up", async () => {
  // The word the C moves at every build (docs/RESTARTS.md), read by the
  // page with one load, and the same number /clockwork/setup would name.
  const { x, memory, arenaBase } = await bootEngine();
  const word = readGeneration(memory.buffer, arenaBase);
  assert.equal(word, 2, "writeArenaHeader put 1 (no instance); dsp_new made it even");
  assert.equal(hasInstance(word), true);
  assert.equal(instanceGeneration(word), 1);
  assert.equal(readArena(memory.buffer, arenaBase).header.generation, 2, "the snapshot carries it too");
  x.clockwork_client_close?.(0);
});

test("the sys rule in bytes agrees with the rule in strings, and a bundle is never claimed", () => {
  const enc = (a) => oscFast.copyEncoded(oscFast.encodeMessage(a, []));
  assert.equal(oscAddress(enc("/dummy/ping")), "/dummy/ping");
  assert.equal(isClockworkSysBytes(enc("/clockwork/ping")), true);
  assert.equal(isClockworkSysBytes(enc("/clockwork-system-status")), false);
  assert.equal(isClockworkSysBytes(enc("/dummy/ping")), false);
  const bundle = oscFast.copyEncoded(oscFast.encodeBundle(2208988801.5, [["/clockwork/ping"]]));
  assert.equal(oscAddress(bundle), "#bundle");
  assert.equal(isClockworkSysBytes(bundle), false);
});

maybe("while there is no instance a message for the guest is refused at the boundary, and clockwork's own goes through", async () => {
  const { x, memory, arenaBase, constants, rings } = await bootEngine();
  const word = generationView(memory.buffer, arenaBase);
  assert.equal(hasInstance(Atomics.load(word, 0)), true, "booted: there is an instance");

  const channel = OscChannel.createSAB({ sharedBuffer: memory.buffer, arenaBase, bufferConstants: constants, sourceId: 7 });
  const refused = [];
  channel.onRefused = (bytes, reason) => refused.push([oscAddress(bytes), reason]);
  const guest = oscFast.copyEncoded(oscFast.encodeMessage("/dummy/ping", []));
  const sys = oscFast.copyEncoded(oscFast.encodeMessage("/clockwork/ping", []));

  // The word odd, as the C turns it at a teardown.
  Atomics.or(word, 0, 1);
  assert.equal(channel.send(guest), false, "not written");
  assert.equal(channel.lastRefusal, REASON_NO_INSTANCE);
  assert.deepEqual(refused, [["/dummy/ping", REASON_NO_INSTANCE]], "the owner heard the word, with the bytes");
  assert.equal(channel.send(sys), true, "clockwork's own goes through");
  assert.equal(channel.lastRefusal, null);
  Atomics.and(word, 0, ~1);

  // The word even again: the same message is written, and the engine answers both.
  assert.equal(channel.send(guest), true);
  assert.equal(channel.lastRefusal, null);
  const heard = [];
  const reader = new EgressReader(rings);
  for (let i = 0; i < 4 && !(heard.includes("/clockwork/pong") && heard.includes("/dummy/pong")); i++) {
    block(x);
    reader.poll(0, (bytes) => heard.push(oscAddress(bytes)));
  }
  assert.ok(heard.includes("/clockwork/pong"), `the ping that went through while there was no instance is answered: ${heard}`);
  assert.ok(heard.includes("/dummy/pong"), `the guest's, sent once there was one: ${heard}`);
  assert.equal(refused.length, 1, "nothing else was refused");
  assert.notEqual(REASON_RING_FULL, REASON_NO_INSTANCE);
  channel.close();
  x.clockwork_client_close?.(0);
});
