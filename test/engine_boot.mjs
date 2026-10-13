// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// A real engine, booted in node, for the tests that need one: the module the
// browser runs, over the memory the worklet lays out, with nothing of the
// browser around it. Not a test itself (no .test. in the name).
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { MemoryLayout } from "../js/memory_layout.js";
import { readArena, REGION } from "../js/lib/arena.js";
import { engineRings } from "../js/lib/ring.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = path.join(here, "..", "dist", "wasm", "clockwork.wasm");

// The tests run against whatever scripts/build-web.sh last produced. Without
// it there is nothing to test, and skipping says so.
export const haveWasm = fs.existsSync(wasmPath);

let compiled = null;

/**
 * Boot an engine and open the C client on its own instance, as the worklet
 * does.
 *
 * @returns {{ x: object, memory: WebAssembly.Memory, ringBufferBase: number,
 *             constants: object, rings: object, handle: number, status: number }}
 *   `x` the engine's exports; `rings` its three rings for js/lib/ring.js;
 *   `handle` its C client; `status` four bytes of its memory for out-params
 */
export async function bootEngine() {
  const wasmModule = compiled ?? (compiled = await WebAssembly.compile(fs.readFileSync(wasmPath)));

  // The worklet's own geometry (js/memory_layout.js). Booting with less is
  // not a smaller test, it is a host the engine has never had: clockwork_init
  // hands the guest its region and cannot boot without one.
  const L = MemoryLayout;
  const page = 65536;
  const memory = new WebAssembly.Memory({
    initial: Math.ceil(L.totalMemory / page),
    maximum: Math.ceil(L.maxTotalMemory / page),
    shared: true,
  });
  const noop = () => 0;
  const engine = await WebAssembly.instantiate(wasmModule, {
    env: {
      memory,
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
  });
  const x = engine.exports;
  x.__wasm_call_ctors?.();
  x.clockwork_init(
    48000, 128, 0, 2, 0,
    L.guestMemoryOffset, L.guestMemorySize, 0, 0, L.memArenaSize,
    L.inboxOffset, L.inboxSize, L.outboxOffset, L.outboxSize);
  const ringBufferBase = x.get_ring_buffer_base();

  // The arena's own table, read from the front of the arena exactly as the
  // worklet reads it (js/lib/arena.js), and held to what ARENA.md says of it.
  const arena = readArena(memory.buffer, ringBufferBase);
  const constants = arena.constants;
  assert.equal(constants.MESSAGE_MAGIC, 0xDEADBEEF, "the ring's frame magic is in the table");
  assert.equal(arena.header.blockBytes, arena.header.guestOffset, "the guest region starts where the block ends");
  assert.ok(arena.region(REGION.IN_RING).offset >= arena.header.headerBytes, "no region overlaps the header");
  // Audiences: the contracts a client reads by hand are published and sit in
  // each half's published run; the command plane is transport, after it.
  assert.ok(arena.published(REGION.METRICS), "metrics are a published contract");
  assert.ok(arena.published(REGION.CLOCK_STATE), "the clock state is a published contract");
  assert.ok(arena.published(REGION.RAW_SCOPE), "the raw scope is a published contract");
  assert.ok(arena.published(REGION.GUEST_WINDOW), "the guest window is a published contract");
  assert.ok(!arena.published(REGION.IN_RING), "the ingress ring is transport");
  assert.ok(!arena.published(REGION.CONTROL), "the control block is transport");
  assert.ok(arena.header.blockPublishedEnd > arena.header.headerBytes, "the block's published run is stated");
  assert.ok(arena.region(REGION.METRICS).offset + arena.region(REGION.METRICS).bytes <= arena.header.blockPublishedEnd);
  assert.ok(arena.region(REGION.CONTROL).offset >= arena.header.blockPublishedEnd);

  const status = x.malloc(4);
  const handle = x.clockwork_client_open_memory(ringBufferBase, constants.TOTAL_BUFFER_SIZE, status);
  assert.ok(handle, "the engine's own client opened");

  return { x, memory, ringBufferBase, constants, rings: engineRings(memory.buffer, ringBufferBase, constants), handle, status };
}

/** Run one block of the engine: the tick, then the audio. */
export function block(x) {
  x.clockwork_tick(0);
  x.process_audio(128);
}
