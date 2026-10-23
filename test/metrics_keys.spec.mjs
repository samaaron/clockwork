// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/**
 * getMetrics() reports every metric the schema names.
 *
 * The schema (js/lib/metrics_schema.js) is where a metric is declared, and
 * the reader (js/lib/metrics_reader.js) is where getMetrics() is built, by
 * hand. A metric added to one and not the other is a panel row that reads
 * undefined, or a value no client can ask for. Clockwork's own suite did not
 * compare the two, and a new counter (egressFramesRefused, slot 55) reached
 * the schema without the reader on 2026-10-16; SuperSonic's suite caught it.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("getMetrics() has a key for every metric the schema declares, and none it does not", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    const metrics = cw.getMetrics();
    const schema = window.Clockwork.getMetricsSchema();
    await cw.destroy();
    // The ring levels are reported as one object per ring (used, peak and
    // capacity together) rather than as their raw words.
    const inRingObjects = new Set([
      "inBufferUsedBytes", "outBufferUsedBytes", "nrtOutBufferUsedBytes",
      "inBufferPeakBytes", "outBufferPeakBytes", "nrtOutBufferPeakBytes",
      "inBufferCapacity", "outBufferCapacity", "nrtOutBufferCapacity",
    ]);
    const ringObjects = new Set(["inBufferUsed", "outBufferUsed", "nrtOutBufferUsed"]);
    const declared = Object.keys(schema.metrics).filter((k) => !inRingObjects.has(k));
    const reported = Object.keys(metrics).filter((k) => !ringObjects.has(k));
    return {
      missing: declared.filter((k) => !reported.includes(k)),
      undeclared: reported.filter((k) => !declared.includes(k)),
    };
  }, clockworkConfig);
  expect(r.missing, "schema metrics getMetrics() does not report").toEqual([]);
  expect(r.undeclared, "getMetrics() keys the schema does not declare").toEqual([]);
});
