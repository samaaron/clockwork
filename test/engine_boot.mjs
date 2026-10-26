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
import { readArena, REGION, AREA, SPACE } from "../js/lib/arena.js";
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
 * @returns {{ x: object, memory: WebAssembly.Memory, arenaBase: number,
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
    L.assetsOffset, L.assetsSize, L.returnsOffset, L.returnsSize);
  const arenaBase = x.get_arena_base();

  // The arena's own table, read from the front of the arena exactly as the
  // worklet reads it (js/lib/arena.js), and held to what ARENA.md says of it.
  const arena = readArena(memory.buffer, arenaBase);
  const constants = arena.constants;
  assert.equal(constants.MESSAGE_MAGIC, 0xDEADBEEF, "the ring's frame magic is in the table");
  assert.equal(arena.header.clockworkBytes, arena.header.guestOffset, "the guest's half starts where clockwork's ends");
  assert.ok(arena.region(REGION.CLOCKWORK_IN_RING).offset >= arena.header.headerBytes, "no region overlaps the header");
  // The areas and the facts: what a client reads by hand is public and lies
  // in a public area; the rings are transport, in the transport area.
  assert.ok(arena.public(REGION.CLOCKWORK_METRICS), "metrics are public");
  assert.ok(arena.public(REGION.CLOCKWORK_CLOCK_STATE), "the clock state is public");
  assert.ok(arena.public(REGION.CLOCKWORK_SCOPE), "the scope is public");
  assert.ok(arena.public(REGION.CLOCKWORK_TAPS), "the taps are public");
  assert.ok(arena.public(REGION.GUEST_WINDOW), "the guest window is public");
  assert.ok(!arena.public(REGION.CLOCKWORK_IN_RING), "the ingress ring is transport");
  assert.ok(!arena.public(REGION.CLOCKWORK_RING_CURSORS), "the ring cursors are transport");
  assert.equal(arena.region(REGION.CLOCKWORK_METRICS).area, AREA.CLOCKWORK_STATE);
  assert.equal(arena.region(REGION.CLOCKWORK_TAPS).area, AREA.CLOCKWORK_AUDIO);
  assert.equal(arena.region(REGION.CLOCKWORK_IN_RING).area, AREA.CLOCKWORK_TRANSPORT);
  assert.equal(arena.region(REGION.GUEST_PERSIST).area, AREA.GUEST_PRIVATE);
  // The three areas outside the arena are placed by the time the engine is
  // up: on the web, as addresses in wasm memory.
  for (const id of [AREA.ASSETS, AREA.RETURNS, AREA.GUEST_MEMORY_BULK, AREA.STATIC_DATA, AREA.STACK, AREA.MALLOC, AREA.ENGINE_MEMORY]) assert.equal(arena.area(id)?.space, SPACE.PROCESS, `area ${id} is placed`);
  // The engine's memory: one block in what malloc took, with the arena at its start and the guest's bulk memory
  // inside it. The static data holds the program's globals and not the arena.
  const sd = arena.area(AREA.STATIC_DATA), ml = arena.area(AREA.MALLOC), gb = arena.area(AREA.GUEST_MEMORY_BULK), em = arena.area(AREA.ENGINE_MEMORY);
  assert.equal(em.base, arenaBase, "the arena is the start of the engine's memory");
  assert.ok(ml.base <= em.base && em.base + em.bytes <= ml.base + ml.bytes, "the engine's memory is in what malloc took");
  assert.ok(em.base <= gb.base && gb.base + gb.bytes <= em.base + em.bytes, "the guest's bulk memory is in the engine's memory");
  assert.ok(sd.base + sd.bytes <= arenaBase, "the arena is not in the static data");
  assert.ok(ml.base <= gb.base && gb.base + gb.bytes <= ml.base + ml.bytes, "the guest's bulk memory is in what malloc took");
  // The RT router's block: armed at boot, so placed, and in what malloc took.
  const rh = arena.area(AREA.RUST_HEAP);
  assert.equal(rh?.space, SPACE.PROCESS, "the rust heap is placed");
  assert.ok(ml.base <= rh.base && rh.base + rh.bytes <= ml.base + ml.bytes, "the rust heap is in what malloc took");
  // The guest's fast memory is placed when the guest asked for any (the dummy does; a guest built with none says so).
  const gm = arena.area(AREA.GUEST_MEMORY_FAST);
  assert.ok(gm && (gm.space === SPACE.PROCESS || (gm.space === SPACE.UNKNOWN && gm.bytes === 0)), "the guest's memory is placed, or none was asked for");
  assert.equal(arena.area(AREA.ASSETS).base, L.assetsOffset);
  assert.equal(arena.area(AREA.RETURNS).base, L.returnsOffset);

  const status = x.malloc(4);
  const handle = x.clockwork_client_open_memory(arenaBase, constants.ARENA_SIZE, status);
  assert.ok(handle, "the engine's own client opened");

  bootedMemory = memory; blockTime = 0; blockFrames = 0;
  return { x, memory, arenaBase, constants, rings: engineRings(memory.buffer, arenaBase, constants), handle, status };
}

/** Run one block of the engine: the tick, passing this host's frame count and its own time as the wall clock
 *  (the NTP epoch plus the audio rendered so far, like a self-driven host), so the engine never sees a gap. */
let blockTime = 0, blockFrames = 0, bootedMemory = null;
export function block(x) {
  // ClockworkBlockTime (lanes.h), 64 bytes in the engine's memory (the last booted: a test boots one engine):
  // u32 struct_bytes, u32 output_latency_frames (0), u64 audio_frames, f64 wall_ntp, f64 monotonic_secs (0),
  // u64 link_host_micros (0), then the engine's outputs block_ntp and block_host_micros.
  blockTime ||= x.clockwork_worklet_alloc(64);
  const words = new Uint32Array(bootedMemory.buffer, blockTime, 16).fill(0);
  words[0] = 64;
  words[2] = blockFrames >>> 0;
  words[3] = Math.floor(blockFrames / 4294967296) >>> 0;
  new Float64Array(bootedMemory.buffer, blockTime + 16, 1)[0] = 2208988800 + blockFrames / 48000;
  x.clockwork_tick(blockTime, 2, 0);
  blockFrames += 128;
}
