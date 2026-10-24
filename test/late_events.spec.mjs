// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * A timed event that comes due late. By default it plays, late: the engine
 * drops nothing for lateness. A page may set the latest a timed event may
 * play (schedulerLateLimitMs); a later one is then dropped and counted as
 * such (engineSchedulerLateDropped), apart from the want-of-room drops.
 */
import { test, expect, boot } from "./fixtures.mjs";

const lateBundle = `async (cw, secondsLate) => {
  const heard = [];
  cw.on("in:osc", ({ oscData }) => { const m = window.Clockwork.osc.decode(oscData); if (Array.isArray(m)) heard.push(m[0]); });
  const wall = () => (performance.timeOrigin + performance.now()) / 1000 + 2208988800;
  cw.sendOSC(window.Clockwork.osc.encodeBundle(wall() - secondsLate, [["/dummy/events/get"]]));
  await cw.sync();
  await new Promise((res) => setTimeout(res, 300));
  const m = cw.getMetrics();
  return { played: heard.filter((a) => a === "/dummy/events").length, lateDropped: m.engineSchedulerLateDropped, dropped: m.engineSchedulerDropped };
}`;

test("a timed event that comes due late plays, late, by default", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, src]) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    const r = await eval(src)(cw, 2.0);
    await cw.shutdown();
    return r;
  }, [clockworkConfig, lateBundle]);
  expect(r.played, "played, late").toBe(1);
  expect(r.lateDropped).toBe(0);
  expect(r.dropped).toBe(0);
});

test("with the latest a timed event may play set by the page, a later one is dropped and counted as such", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, src]) => {
    const cw = new window.Clockwork({ ...config, schedulerLateLimitMs: 500 });
    await cw.init();
    const within = await eval(src)(cw, 0.2);
    const beyond = await eval(src)(cw, 2.0);
    await cw.shutdown();
    return { within, beyond };
  }, [clockworkConfig, lateBundle]);
  expect(r.within.played, "within the limit: played").toBe(1);
  expect(r.within.lateDropped).toBe(0);
  expect(r.beyond.played, "beyond it: not played").toBe(0);
  expect(r.beyond.lateDropped, "counted as late and dropped").toBe(1);
  expect(r.beyond.dropped, "not as want of room").toBe(0);
});
