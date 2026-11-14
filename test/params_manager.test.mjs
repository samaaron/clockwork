// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * params_manager.js against the real Rust core (dist/params, built by
 * scripts/build-web-subsystems.sh), on the fixture the native test writes: a page
 * says what clockwork-params's own tests say a plugin host's bridge says.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { readParams } from "../js/lib/params.js";
import { ParamConversions } from "../js/lib/params_manager.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = path.join(here, "..", "dist", "wasm", "clockwork_params_bg.wasm");

const table = () => {
  const bytes = fs.readFileSync(path.join(here, "fixtures", "params_region.bin"));
  const buffer = new ArrayBuffer(bytes.length);
  new Uint8Array(buffer).set(bytes);
  return readParams(buffer, 0);
};
const convert = () => new ParamConversions({ wasm: fs.readFileSync(wasmPath) }).init();

test("a value's place on a knob comes from the entry's taper", async () => {
  const c = await convert();
  const cutoff = table().entries.find((e) => e.id === 1);
  assert.ok(Math.abs(c.positionOf(cutoff, 632.456) - 0.5) < 1e-4, "the middle of 20 Hz to 20 kHz, on a log sweep");
  assert.ok(Math.abs(c.valueAt(cutoff, 0.5) - 632.456) < 0.01);
});

test("a value's words come from its facts and text, and read back", async () => {
  const c = await convert();
  const t = table();
  const of = (id) => t.entries.find((e) => e.id === id);
  assert.equal(c.textOf(of(1), 440), "440 Hz");
  assert.equal(c.textOf(of(1), 1800), "1.80 kHz");
  assert.equal(c.textOf(of(2), 3), "3 steps");
  assert.equal(c.textOf(of(3), 1), "On");
  assert.equal(c.textOf(of(4), 2), "é");
  assert.equal(c.valueOfText(of(1), "1.2 kHz"), 1200);
  assert.equal(c.valueOfText(of(4), "SQUARE"), 1);
  assert.equal(c.valueOfText(of(1), "loud"), undefined);
});
