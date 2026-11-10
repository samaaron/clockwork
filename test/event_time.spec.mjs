// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * The time a message to or from the engine was seen (out:osc, in:osc, and their text and html lines) is on the
 * engine's clock: the one bundles are stamped on and clock.now() reads, so a host can put the two side by side (a
 * bundle late when its time had passed as it went in; SuperSonic's guide, "Latency Analysis with in:osc"). Read from
 * the wall clock it was out by however far the two had drifted apart (a stall leaves the engine's clock behind the
 * wall until its offset is slewed back), and a bundle sent in good time looked late. A global clock offset of ten
 * seconds sets the two clocks apart beyond any doubt.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("a message's time, going to the engine or coming from it, is on the engine's clock", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    cw.setClockOffset(10);
    // the offset is in the engine's clock from its next block
    for (let i = 0; i < 100 && cw.clock.now() - cw.clock.wallNow() < 5; i++) await sleep(20);
    const seen = { out: [], in: [], html: [] };
    cw.on("out:osc", (d) => seen.out.push({ seen: d.timestamp, now: cw.clock.now(), scheduled: d.scheduledTime }));
    cw.on("in:osc", (d) => seen.in.push({ seen: d.timestamp, now: cw.clock.now() }));
    cw.on("in:html", (d) => seen.html.push(d.html));
    cw.on("out:html", (d) => seen.html.push(d.html));
    cw.sendOSC(window.Clockwork.osc.encodeBundle(cw.clock.now() + 0.3, [["/dummy/ping"]]));
    await cw.sync(77);
    await sleep(600);
    return { apart: cw.clock.now() - cw.clock.wallNow(), seen };
  }, clockworkConfig);

  expect(r.apart).toBeGreaterThan(9);
  expect(r.seen.out.length).toBeGreaterThan(0);
  expect(r.seen.in.length).toBeGreaterThan(0);
  // seen at the engine's now as the event arrived, give or take its delivery: not ten seconds behind it
  for (const m of [...r.seen.out, ...r.seen.in]) expect(Math.abs(m.now - m.seen)).toBeLessThan(1);
  // the bundle sent 300 ms ahead was seen going in ahead of its time, as it was
  const timed = r.seen.out.find((m) => m.scheduled);
  expect(timed).toBeTruthy();
  expect(timed.scheduled - timed.seen).toBeGreaterThan(0.1);
  // the lines' time column: seconds since the session began, never below nought
  const times = r.seen.html.map((h) => /clockwork-osc-time">([^<]*)</.exec(h)?.[1]).filter((t) => t != null);
  expect(times.length).toBeGreaterThan(0);
  for (const t of times) expect(t).toMatch(/^\d+\.\d\d$/);
});
