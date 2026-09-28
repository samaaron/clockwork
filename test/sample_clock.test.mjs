// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * The sample clock's JS reader (js/lib/sample_clock.js) against the region as the audio thread writes it
 * (ClockworkClock::publishSampleClock): the offsets are the C++ header's, a reading is taken only between two equal,
 * even seqs, and a region never published reads as nothing.
 *
 *   node --test test/sample_clock.test.mjs
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import * as SampleClock from "../js/lib/sample_clock.js";

const here = dirname(fileURLToPath(import.meta.url));

// what publishSampleClock writes: seq odd, the fields, seq even
function publish(buffer, offset, { frames, sampleRate, renderNtp, latency = 0 }, { leaveOdd = false } = {}) {
  const u32 = new Uint32Array(buffer, offset, 8), dv = new DataView(buffer, offset, 32);
  const s = u32[0];
  u32[0] = s + 1;
  u32[1] = sampleRate;
  dv.setBigUint64(8, BigInt(frames), true);
  dv.setFloat64(16, renderNtp + latency / sampleRate, true);
  u32[6] = latency;
  if (!leaveOdd) u32[0] = s + 2;
}

test("the reader's offsets are the engine's (src/shared_memory.h SAMPLE_CLOCK_*)", () => {
  const header = readFileSync(join(here, "..", "src", "shared_memory.h"), "utf8");
  for (const name of ["SAMPLE_CLOCK_SEQ", "SAMPLE_CLOCK_SAMPLE_RATE", "SAMPLE_CLOCK_ENGINE_FRAMES", "SAMPLE_CLOCK_DAC_NTP", "SAMPLE_CLOCK_OUT_LATENCY"]) {
    const m = new RegExp(`constexpr uint32_t ${name}\\s*=\\s*(\\d+);`).exec(header);
    assert.ok(m, `${name} is in the header`);
    assert.equal(SampleClock[name], Number(m[1]), name);
  }
});

test("a published block reads back, its render time the DAC time less the latency", () => {
  const buffer = new SharedArrayBuffer(64), read = SampleClock.sampleClockReader(buffer, 16);
  assert.equal(read(), null, "never published: nothing");
  publish(buffer, 16, { frames: 5 * 2 ** 32 + 123, sampleRate: 48000, renderNtp: 3999577665.25, latency: 480 });
  const into = {};
  const r = read(into);
  assert.equal(r, into, "the caller's object, filled");
  assert.equal(r.frames, 5 * 2 ** 32 + 123);
  assert.equal(r.sampleRate, 48000);
  assert.equal(r.outputLatencyFrames, 480);
  assert.equal(r.dacNtp, 3999577665.25 + 0.01);
  assert.ok(Math.abs(r.renderNtp - 3999577665.25) < 1e-9);
});

test("a write under way is not read: the reading before it or nothing, never half of each", () => {
  const buffer = new SharedArrayBuffer(64), read = SampleClock.sampleClockReader(buffer, 0);
  publish(buffer, 0, { frames: 128, sampleRate: 44100, renderNtp: 100 }, { leaveOdd: true });
  assert.equal(read(), null, "the seq is odd: a writer is mid-update");
  new Uint32Array(buffer, 0, 1)[0] += 1;   // the writer finishes
  assert.equal(read().renderNtp, 100);
});

test("a wasm memory that grows is read through its new buffer", () => {
  let buffer = new ArrayBuffer(64);
  const read = SampleClock.sampleClockReader(() => buffer, 32);
  publish(buffer, 32, { frames: 256, sampleRate: 48000, renderNtp: 7 });
  assert.equal(read().renderNtp, 7);
  const grown = new ArrayBuffer(128);
  new Uint8Array(grown).set(new Uint8Array(buffer));
  buffer = grown;
  publish(buffer, 32, { frames: 384, sampleRate: 48000, renderNtp: 8 });
  assert.equal(read().renderNtp, 8);
});
