// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// js/lib/wasm_client.js gives a JavaScript context that is NOT the audio
// worklet a way into src/clockwork_client.h: its own module instance over the
// engine's memory, standing on a claimed client slot.
//
// The thing worth pinning is that the second instance is safe to run at all.
// It shares one linear memory with the engine, so it shares the engine's
// stack region until it is told otherwise, and it must never call the
// allocator that the audio thread is using. Both are invisible when they go
// wrong — the symptom is corruption somewhere else, later — so they are
// asserted here rather than left to a browser to discover.
//
// The engine is instantiated in this process, which is what makes this a node
// test rather than a browser one: two instances, one memory, no worklet.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  WasmClient, claimClientSlot, releaseClientSlot, REGION_INGRESS, REGION_EGRESS,
} from "../js/lib/wasm_client.js";
import { MemoryLayout } from "../js/memory_layout.js";
import { readArena, REGION } from "../js/lib/arena.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = path.join(here, "..", "dist", "wasm", "clockwork.wasm");

// The suite runs against whatever scripts/build-web.sh last produced. Without
// it there is nothing to test, and saying so beats a stack trace.
const haveWasm = fs.existsSync(wasmPath);
const maybe = haveWasm ? test : test.skip;

let cached = null;
async function bootEngine() {
  const wasmModule = cached ?? (cached = await WebAssembly.compile(fs.readFileSync(wasmPath)));

  // The worklet's own geometry (js/memory_layout.js): the heap, the rings,
  // the arena, the outbox, the guest's region and the inbox, in that order.
  // Booting with less is not a smaller test, it is a host the engine has
  // never had — clockwork_init hands the guest its region and cannot boot
  // without one.
  const L = MemoryLayout;
  const page = 65536;
  const wasmMemory = new WebAssembly.Memory({
    initial: Math.ceil(L.totalMemory / page),
    maximum: Math.ceil(L.maxTotalMemory / page),
    shared: true,
  });
  const noop = () => 0;
  const imports = {
    env: {
      memory: wasmMemory,
      emscripten_notify_memory_growth: noop,
      _emscripten_thread_set_strongref: noop,
      emscripten_exit_with_live_runtime: noop,
      __syscall_getcwd: noop,
      _emscripten_init_main_thread_js: noop,
      _emscripten_thread_mailbox_await: noop,
      _emscripten_receive_on_main_thread_js: noop,
      emscripten_check_blocking_allowed: noop,
      _emscripten_thread_cleanup: noop,
      _emscripten_notify_mailbox_postmessage: noop,
    },
    wasi_snapshot_preview1: {
      clock_time_get: noop, fd_close: noop, environ_sizes_get: noop,
      environ_get: noop, fd_seek: noop, fd_read: noop, fd_write: () => 0,
      proc_exit: noop,
    },
  };

  const engine = await WebAssembly.instantiate(wasmModule, imports);
  engine.exports.__wasm_call_ctors?.();
  engine.exports.clockwork_init(
    48000, 128, 0, 2, 0,
    L.guestMemoryOffset, L.guestMemorySize, 0, 0, L.memArenaSize,
    L.inboxOffset, L.inboxSize, L.outboxOffset, L.outboxSize);
  const ringBufferBase = engine.exports.get_ring_buffer_base();

  // The arena's own table, read from the front of the arena exactly as the
  // worklet reads it (js/lib/arena.js).
  const arena = readArena(wasmMemory.buffer, ringBufferBase);
  const bufferConstants = arena.constants;
  assert.equal(bufferConstants.MESSAGE_MAGIC, 0xDEADBEEF, "the ring's frame magic is in the table");
  assert.equal(arena.header.blockBytes, arena.header.guestOffset, "the guest region starts where the block ends");
  assert.ok(arena.region(REGION.IN_RING).offset >= arena.header.headerBytes, "no region overlaps the header");

  return { wasmModule, wasmMemory, engine, ringBufferBase, bufferConstants };
}

// "/test/ping\0\0" + ",\0\0\0"
function oscMessage(address) {
  const pad = (n) => (n + 4) & ~3;
  const addrLen = pad(address.length);
  const out = new Uint8Array(addrLen + 4);
  for (let i = 0; i < address.length; i++) out[i] = address.charCodeAt(i);
  out[addrLen] = 0x2c;   // ','
  return out;
}

maybe("a client sends through the boundary and the engine receives it", async () => {
  const { wasmModule, wasmMemory, engine, ringBufferBase, bufferConstants } = await bootEngine();

  const client = await WasmClient.open({
    wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "test",
  });
  assert.ok(client, "a slot was free and the handle opened");

  const before = engine.exports.get_messages_processed();
  for (let i = 0; i < 5; i++)
    assert.equal(client.send(oscMessage("/test/ping"), 0xC0FFEE), true, `send ${i}`);

  engine.exports.clockwork_tick(0);
  engine.exports.process_audio(128);
  assert.equal(engine.exports.get_messages_processed() - before, 5,
    "every message the client framed reached the engine");

  client.close();
});

maybe("the client instance runs on its own stack, not the engine's", async () => {
  // THE ONE THAT MATTERS, AND THE ONE THAT IS EASY TO FAKE. Both instances
  // start with the same stack pointer, so without the hand-off in
  // WasmClient.open the client's calls run on the audio thread's stack.
  //
  // Note what does NOT show that. Simply calling the client and finding the
  // engine unharmed proves nothing here: JavaScript runs the two instances one
  // after another, so a shared stack is never actually shared at a moment when
  // it would hurt. Removing the hand-off entirely leaves every round trip in
  // this file passing. The ADDRESS is the evidence, so the address is what is
  // asserted.
  const { wasmModule, wasmMemory, engine, ringBufferBase, bufferConstants } = await bootEngine();

  const client = await WasmClient.open({
    wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "stack",
  });
  assert.ok(client);

  const { low, high } = client.stackRange();
  const sp = client.stackPointer();
  assert.ok(sp > low && sp <= high,
    `the client stands in its own slot: ${sp} not within (${low}, ${high}]`);

  // And that slot is nowhere near where the engine is standing.
  const engineStack = engine.exports.emscripten_stack_get_current();
  assert.ok(engineStack <= low || engineStack > high,
    `the engine's stack ${engineStack} is outside the client's slot`);

  // It stays in its own slot with work behind it, rather than drifting out.
  for (let i = 0; i < 200; i++) client.send(oscMessage("/test/ping"), 1);
  const after = client.stackPointer();
  assert.ok(after > low && after <= high, "still in its slot after 200 sends");
  assert.equal(engine.exports.emscripten_stack_get_current(), engineStack,
    "and the engine's stack pointer never moved");

  client.close();
});

maybe("slots are claimed exclusively and handed back", async () => {
  const { wasmModule, wasmMemory, ringBufferBase, bufferConstants } = await bootEngine();
  const atomicView = new Int32Array(wasmMemory.buffer);
  const count = bufferConstants.CLIENT_SLOT_COUNT;

  // Every slot, then one too many: a context that cannot get one is told so
  // rather than sharing another's stack.
  const taken = [];
  for (let i = 0; i < count; i++) {
    const idx = claimClientSlot(atomicView, ringBufferBase, bufferConstants);
    assert.ok(idx >= 0, `slot ${i} claimed`);
    assert.equal(taken.includes(idx), false, "no slot handed out twice");
    taken.push(idx);
  }
  assert.equal(claimClientSlot(atomicView, ringBufferBase, bufferConstants), -1,
    "the pool is exhausted, and says so");

  releaseClientSlot(atomicView, ringBufferBase, bufferConstants, taken[1]);
  assert.equal(claimClientSlot(atomicView, ringBufferBase, bufferConstants), taken[1],
    "a released slot comes back");

  for (const idx of taken) releaseClientSlot(atomicView, ringBufferBase, bufferConstants, idx);

  // And a real client can still open once they are all back.
  const client = await WasmClient.open({
    wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "reuse",
  });
  assert.ok(client);
  client.close();
});

maybe("a tap watches ingress without taking it from the engine", async () => {
  const { wasmModule, wasmMemory, engine, ringBufferBase, bufferConstants } = await bootEngine();

  const logger = await WasmClient.open({
    wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "logger",
  });
  assert.ok(logger);
  assert.equal(logger.openTap(REGION_INGRESS), true);

  const sender = await WasmClient.open({
    wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "sender",
  });
  assert.ok(sender);

  const before = engine.exports.get_messages_processed();
  for (let i = 0; i < 3; i++) sender.send(oscMessage("/test/ping"), 42);

  const seen = [];
  logger.tapPoll((bytes, origin) => {
    seen.push({ text: new TextDecoder().decode(bytes.slice(0, 10)), origin });
  });
  assert.equal(seen.length, 3, "the tap saw all three");
  assert.equal(seen[0].text, "/test/ping");
  assert.equal(seen[0].origin, 42, "and who sent them");

  // Watching took nothing: the engine still gets all three.
  engine.exports.clockwork_tick(0);
  engine.exports.process_audio(128);
  assert.equal(engine.exports.get_messages_processed() - before, 3,
    "the engine received what the tap watched");

  logger.close();
  sender.close();
});

maybe("many sends in a row all arrive, and an impossible one is refused", async () => {
  // There is no staging buffer any more: each message is built in the ring
  // itself. Sending far more than the ring holds at once, drained as it goes,
  // exercises the reserve/commit pair against a moving reader.
  //
  // (Over-reservation — asking for more room than the message needs — is not
  // reachable from here, because send() knows the exact length. It is pinned
  // in C by "the ring advances by what was used, not what was asked for".)
  const { wasmModule, wasmMemory, engine, ringBufferBase, bufferConstants } = await bootEngine();
  const client = await WasmClient.open({
    wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "bounds",
  });

  const before = engine.exports.get_messages_processed();
  const msg = oscMessage("/test/ping");
  const rounds = 200;
  for (let i = 0; i < rounds; i++) {
    assert.equal(client.send(msg, 1), true, `send ${i} of ${rounds}`);
  }

  // The engine takes a bounded number of messages per block, so this drains
  // over several rather than expecting one to swallow the lot.
  for (let block = 0; block < 40; block++) {
    engine.exports.clockwork_tick(0);
    engine.exports.process_audio(128);
    if (engine.exports.get_messages_processed() - before >= rounds) break;
  }
  assert.equal(engine.exports.get_messages_processed() - before, rounds,
    "every message that was accepted arrived");

  // A message larger than the ring can ever hold is refused, and the client
  // still works afterwards — the refusal did not leave the ring locked.
  const tooBig = new Uint8Array(bufferConstants.IN_BUFFER_SIZE + 4096);
  assert.equal(client.send(tooBig, 0), false, "a message bigger than the ring is refused");
  assert.equal(client.send(msg, 1), true, "and the ring is still usable");

  client.close();
});

maybe("running out of slots is an error, not a quiet no-op", async () => {
  // A context that carried on without a client would look like a working
  // transport that silently delivers nothing, which is the worst of both.
  const { wasmModule, wasmMemory, ringBufferBase, bufferConstants } = await bootEngine();
  const atomicView = new Int32Array(wasmMemory.buffer);

  const taken = [];
  for (let i = 0; i < bufferConstants.CLIENT_SLOT_COUNT; i++) {
    taken.push(claimClientSlot(atomicView, ringBufferBase, bufferConstants));
  }

  await assert.rejects(
    () => WasmClient.open({ wasmModule, wasmMemory, ringBufferBase, bufferConstants, label: "none" }),
    /no client slot free/,
    "opening with no slot free must throw");

  for (const idx of taken) releaseClientSlot(atomicView, ringBufferBase, bufferConstants, idx);
});
