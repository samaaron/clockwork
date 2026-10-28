// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * What every counter and gauge did over the last hour, as a page reads it
 * (getMetricsHistory, metrics_history.js): on both transports, a window of
 * the last second shows the blocks that were rendered in it, a constant has
 * no history, and the keys are the schema's non-constant engine metrics.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("getMetricsHistory() shows the last second's blocks, on either transport", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config]) => {
    // The history's minutes are the engine clock's whole minutes, and the engine clock starts on the wall's. A run
    // that straddles one has two of them, rightly, so this one starts clear of a boundary: the case takes under two
    // seconds, and if fewer than five are left in this minute it waits for the next.
    const left = 60000 - (Date.now() % 60000);
    if (left < 5000) await new Promise((res) => setTimeout(res, left + 250));
    const cw = new window.Clockwork(config);
    await cw.init();
    await new Promise((res) => setTimeout(res, 1300));   // a second and a bit of blocks
    const h = cw.getMetricsHistory();
    const schema = window.Clockwork.getMetricsSchema();
    const expected = Object.entries(schema.metrics)
      .filter(([, d]) => d.offset < 80 && !d.webOnly && d.type !== "constant").map(([k]) => k).sort();
    const got = Object.keys(h).sort();
    const pc = h.engineProcessCount;
    await cw.shutdown();
    return { got, expected, pc, sampleRate: h.audioSampleRate, mode: config.mode };
  }, [clockworkConfig]);
  expect(r.got, "the metrics with a history are the schema's non-constant engine metrics").toEqual(r.expected);
  expect(r.sampleRate, "a constant has no history").toBeUndefined();
  expect(r.pc.s1.covered, "the current second recorded").toBe(1);
  expect(r.pc.s1.delta, `blocks so far this second (${JSON.stringify(r.pc)})`).toBeGreaterThanOrEqual(1);
  expect(r.pc.s10.covered, "two seconds begun").toBeGreaterThanOrEqual(2);
  expect(r.pc.s10.delta, `blocks in the last ten seconds: over a second of them (${JSON.stringify(r.pc)})`).toBeGreaterThan(300);
  expect(r.pc.s10.last, "the last value is the count now").toBeGreaterThanOrEqual(r.pc.s1.last);
  expect(r.pc.h1.covered, "the hour has one minute so far").toBe(1);
});
