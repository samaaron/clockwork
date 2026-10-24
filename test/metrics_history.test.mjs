// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The page's reader and the engine's writer number the metrics' history slots
// the same way: both from the schema, and the engine's table is generated
// into src/metrics_schema.h — read here and compared.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { historySlots, HISTORY_WINDOWS } from "../js/lib/metrics_history.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");

test("the history slots match the engine's generated table", () => {
  const hdr = readFileSync(join(root, "src", "metrics_schema.h"), "utf8");
  const count = Number(hdr.match(/kMetricHistoryCount = (\d+);/)[1]);
  const table = hdr.match(/kMetricHistorySlot\[kMetricWords\] = \{([^}]*)\}/)[1].split(",").map((s) => s.trim()).filter((s) => s !== "").map(Number);
  const slots = historySlots();
  assert.equal(slots.length, count);
  assert.equal(table.length, 80);
  for (const s of slots) assert.equal(table[s.offset], s.slot, `offset ${s.offset} (${s.key})`);
  const withHistory = table.filter((v) => v >= 0).length;
  assert.equal(withHistory, count);
  assert.deepEqual(HISTORY_WINDOWS.map((w) => w.name), ["s1", "s10", "m1", "m10", "h1"]);
});
