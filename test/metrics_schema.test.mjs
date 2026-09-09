// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * The metrics schema has two copies, and this is what keeps them equal.
 *
 *   node --test test/metrics_schema.test.mjs
 *
 * `js/lib/metrics_schema.js` is the source; `src/metrics_schema.h` is
 * generated from it by scripts/gen-metrics-schema-header.mjs and committed, so
 * a native GUI can read metric names and descriptions without shelling out to
 * node. Two files saying the same thing, one written by hand and one by a
 * script that nobody is obliged to run.
 *
 * NOTHING ENFORCED IT until now, and the failure is quiet by construction: a
 * metric added or renumbered in the JS shows up in the web UI immediately and
 * in the native one never, or — worse — the native one keeps reporting the
 * PREVIOUS occupant of a slot under the new name. Removing three native stats
 * on 2026-09-01 renumbered everything after them, which is exactly the edit
 * that would have left the header describing the old layout.
 *
 * So the test is the regeneration itself: render from the JS source and
 * compare, byte for byte, with what is checked in. It cannot drift without
 * saying so, and the fix it names is one command.
 */

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

import { renderHeader, renderRust } from "../scripts/gen-metrics-schema-header.mjs";
import { METRICS_SCHEMA } from "../js/lib/metrics_schema.js";

const here = dirname(fileURLToPath(import.meta.url));
const headerPath = join(here, "..", "src", "metrics_schema.h");
const rustPath = join(here, "..", "rust", "clockwork-client", "src", "metrics_schema.rs");

test("the committed C++ header matches what the JS schema generates", () => {
  const onDisk = readFileSync(headerPath, "utf8");
  const fresh = renderHeader();
  assert.equal(
    onDisk,
    fresh,
    "src/metrics_schema.h is out of date with js/lib/metrics_schema.js — "
      + "run `npm run gen:metrics-header` and commit the result",
  );
});

test("the committed Rust module matches what the JS schema generates", () => {
  // The third copy, for a Rust host reading through clockwork-client. Same
  // failure mode as the header, same fix.
  const onDisk = readFileSync(rustPath, "utf8");
  const fresh = renderRust();
  assert.equal(
    onDisk,
    fresh,
    "rust/clockwork-client/src/metrics_schema.rs is out of date with "
      + "js/lib/metrics_schema.js — run `npm run gen:metrics-header` and commit the result",
  );
});

test("every schema key is a Rust identifier once capitalised", () => {
  // The Rust enum is named from the keys, so a key that is not an identifier
  // (a dash, a leading digit) would generate a module that does not compile —
  // caught here, in the schema's own test, rather than in a Rust build.
  for (const group of ["metrics", "nativeStats"]) {
    for (const key of Object.keys(METRICS_SCHEMA[group])) {
      assert.match(key, /^[a-z][A-Za-z0-9]*$/, `${group}.${key} is not a camelCase identifier`);
    }
  }
});

test("native stat slots are dense and start at zero", () => {
  // The slots are read BY INDEX out of a fixed-size region, so a hole or a
  // duplicate is not a tidiness question: index 3 means "the fourth u32 in
  // NATIVE_STATS", and two entries claiming it means one of them is reporting
  // the other's number under its own name.
  const stats = Object.entries(METRICS_SCHEMA.nativeStats);
  const indices = stats.map(([, d]) => d.index).sort((a, b) => a - b);
  assert.deepEqual(
    indices,
    indices.map((_, i) => i),
    `native stat indices must be 0..${indices.length - 1} with no gaps or `
      + `duplicates, got ${JSON.stringify(indices)}`,
  );
});

test("every metric declares a description and a type, and every measurement a unit", () => {
  // These are shown to a human in two UIs. A missing one is not a crash, it is
  // a blank cell that stays blank because nobody notices theirs is the one.
  //
  // A UNIT IS REQUIRED OF MEASUREMENTS, NOT OF STATES. `enum` metrics —
  // audioContextState, mode — report which of a few named things is currently
  // true, and there is no unit in which "suspended" is a quantity. Demanding
  // one would be answered by inventing a word like "state", which is a unit
  // column that means nothing and reads like one that does.
  for (const [group, entries] of Object.entries(METRICS_SCHEMA)) {
    if (group === "layout" || group === "composites") continue;
    for (const [name, def] of Object.entries(entries)) {
      assert.ok(def.description, `${group}.${name} has no description`);
      assert.ok(def.type, `${group}.${name} has no type`);
      if (def.type !== "enum") {
        assert.ok(def.unit, `${group}.${name} is a ${def.type} and has no unit`);
      }
    }
  }
});
