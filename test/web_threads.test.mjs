// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// A thread outside the worklet, talking to a real engine through
// js/lib/ring.js and nothing else (docs/THREADS.md): it sends, it hears the
// engine answer, and it watches what was sent without taking it. The engine is
// booted in this process, so the round trip is the engine's own C at the far
// end of every ring.
//
// ring_cross.test.mjs holds ring.js to the C frame by frame. This holds the
// whole path: what a channel writes is what the engine processes, and what
// the engine says is what the reply pump reads.
import { test } from "node:test";
import assert from "node:assert/strict";

import { ringWrite, RingTap, EgressReader } from "../js/lib/ring.js";
import { bootEngine, block, haveWasm } from "./engine_boot.mjs";

const maybe = haveWasm ? test : test.skip;

const pad = (n) => (n + 4) & ~3;
function ascii(out, at, text) {
  for (let i = 0; i < text.length; i++) out[at + i] = text.charCodeAt(i);
}

// "/test/ping\0\0" + ",\0\0\0"
function oscMessage(address) {
  const addrLen = pad(address.length);
  const out = new Uint8Array(addrLen + 4);
  ascii(out, 0, address);
  out[addrLen] = 0x2c;   // ','
  return out;
}

// address + ",i" + one big-endian int32
function oscInt(address, value) {
  const addrLen = pad(address.length);
  const out = new Uint8Array(addrLen + 4 + 4);
  ascii(out, 0, address);
  ascii(out, addrLen, ",i");
  new DataView(out.buffer).setInt32(addrLen + 4, value, false);
  return out;
}

maybe("a sender outside the worklet reaches the engine", async () => {
  const { x, rings } = await bootEngine();
  const before = x.get_messages_processed();
  for (let i = 0; i < 5; i++)
    assert.equal(ringWrite(rings.ingress, oscMessage("/test/ping"), 0xC0FFEE), true, `send ${i}`);

  block(x);
  assert.equal(x.get_messages_processed() - before, 5, "every message written reached the engine");
});

maybe("many sends in a row all arrive, and an impossible one is refused", async () => {
  const { x, rings, constants } = await bootEngine();
  const before = x.get_messages_processed();
  const msg = oscMessage("/test/ping");
  const rounds = 200;
  for (let i = 0; i < rounds; i++)
    assert.equal(ringWrite(rings.ingress, msg, 1), true, `send ${i} of ${rounds}`);

  // The engine takes a bounded number of messages per block, so this drains
  // over several rather than expecting one to swallow the lot.
  for (let n = 0; n < 40 && x.get_messages_processed() - before < rounds; n++) block(x);
  assert.equal(x.get_messages_processed() - before, rounds, "every message that was accepted arrived");

  // A message larger than the ring can ever hold is refused, and the ring
  // still works afterwards: the refusal did not leave the lock held.
  const tooBig = new Uint8Array(constants.IN_BUFFER_SIZE + 4096);
  assert.equal(ringWrite(rings.ingress, tooBig, 0), false, "a message bigger than the ring is refused");
  assert.equal(Atomics.load(rings.ingress.words, rings.ingress.lock), 0, "the write lock is free");
  assert.equal(ringWrite(rings.ingress, msg, 1), true, "and the ring is still usable");
});

maybe("the reply pump's reader hears the engine answer", async () => {
  const { x, rings } = await bootEngine();
  const reader = new EgressReader(rings);
  reader.poll(0, () => {});   // whatever the engine said while it booted

  assert.equal(ringWrite(rings.ingress, oscInt("/clockwork/sync", 4242), 0), true);
  block(x);

  const heard = [];
  reader.poll(0, (bytes) => heard.push(bytes.slice()));
  const synced = heard.find((b) => new TextDecoder().decode(b.subarray(0, 17)) === "/clockwork/synced");
  assert.ok(synced, `the engine answered the sync (heard ${heard.length} replies)`);
  const view = new DataView(synced.buffer);
  assert.equal(view.getInt32(synced.length - 4, false), 4242, "with the id it was given");

  // The next poll gives the reply's space back to the engine.
  reader.poll(0, () => {});
  assert.equal(Atomics.load(rings.egress.words, rings.egress.tail),
               Atomics.load(rings.egress.words, rings.egress.read), "the reply's space went back");
});

maybe("a tap watches ingress without taking it from the engine", async () => {
  const { x, rings } = await bootEngine();
  const tap = new RingTap(rings.ingress);

  const before = x.get_messages_processed();
  for (let i = 0; i < 3; i++) ringWrite(rings.ingress, oscMessage("/test/ping"), 42);

  const seen = [];
  tap.poll(0, (origin, offset, length) => {
    seen.push({ text: new TextDecoder().decode(rings.ingress.bytes.slice(offset, offset + 10)), origin });
  });
  assert.equal(seen.length, 3, "the tap saw all three");
  assert.equal(seen[0].text, "/test/ping");
  assert.equal(seen[0].origin, 42, "and who sent them");

  // Watching took nothing: the engine still gets all three.
  block(x);
  assert.equal(x.get_messages_processed() - before, 3, "the engine received what the tap watched");
});
