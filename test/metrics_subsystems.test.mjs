// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// engineSubsystems: the word that says what an engine has. The engine writes
// the bits for what it was built with; in a browser MIDI and game controller
// input are the page's, so the client adds theirs to what it reads.
import { test } from "node:test";
import assert from "node:assert/strict";
import { MetricsReader } from "../js/lib/metrics_reader.js";
import { METRICS_SCHEMA } from "../js/lib/metrics_schema.js";
import * as MetricsOffsets from "../js/lib/metrics_offsets.js";

const def = METRICS_SCHEMA.metrics.engineSubsystems;
const bit = (name) => {
  const i = def.bits.indexOf(name);
  assert.ok(i >= 0, `engineSubsystems has no bit named ${name}`);
  return 1 << i;
};

// A reader fed the engine's block as the worklet posts it, with `word` at
// the subsystems offset.
function readerWith(word) {
  const reader = new MetricsReader({ mode: "postMessage" });
  const block = new Uint32Array(MetricsOffsets.SAB_METRICS_WORDS);
  block[MetricsOffsets.SUBSYSTEMS] = word;
  reader.updateSnapshot(block.buffer);
  return reader;
}

test("the schema names each bit once, at the offset the reader uses", () => {
  assert.equal(def.type, "flags");
  assert.equal(def.offset, MetricsOffsets.SUBSYSTEMS);
  assert.deepEqual(def.bits, ["midi", "gamepad", "osc", "link", "linkAudio", "scheduler"]);
  assert.equal(new Set(def.bits).size, def.bits.length);
});

test("the engine's bits are read as it wrote them", () => {
  const metrics = readerWith(bit("scheduler") | bit("osc")).gatherMetrics();
  assert.equal(metrics.engineSubsystems, bit("scheduler") | bit("osc"));
});

test("the page's subsystems are added to the engine's", () => {
  const reader = readerWith(bit("scheduler"));
  const metrics = reader.gatherMetrics({ subsystems: bit("gamepad") });
  assert.equal(metrics.engineSubsystems, bit("scheduler") | bit("gamepad"));
  // ...and are gone again once the page no longer has them.
  assert.equal(reader.gatherMetrics().engineSubsystems, bit("scheduler"));
});
