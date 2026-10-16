// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * The engine's clock in a worker: OscChannel#now.
 *
 * A worker cannot see the AudioContext, so a host that schedules from one (Sonic Pi's runtime) was posted the time by
 * its page once a second, and counted on from it. That time went stale: a phone's audio away for six seconds, the
 * engine rebuilt on the tap, and the next Run started six seconds late. The channel now reads the audio thread's own
 * time — the sample clock it publishes (SAB), or its word down the channel's port (postMessage) — and this is what
 * that time must be: the page's engine clock, standing still with the audio, and following it back.
 */
import { test, expect, boot } from "./fixtures.mjs";

// A promise raced against a deadline: a call that never settles must fail the test rather than time the run out
const within = `(p, ms) => Promise.race([p.then((v) => ({ settled: true, value: v }), (e) => ({ settled: true, error: String(e?.message ?? e) })), new Promise((r) => setTimeout(() => r({ settled: false }), ms))])`;

test("a worker's channel reads the engine's clock: with the page's, still while the audio is, and on after a resume and a reload", async ({ page, clockworkConfig, clockworkMode }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc, mode]) => {
    const within = eval(withinSrc);
    const src = `
      globalThis.__DEV__ = false;
      let channel = null;
      self.onmessage = async ({ data }) => {
        if (data.type === "channel") {
          const { OscChannel } = await import("${location.origin}/js/lib/osc_channel.js");
          channel = await OscChannel.fromTransferable(data.channel);
          return self.postMessage({ type: "ready" });
        }
        if (data.type === "now") self.postMessage({ type: "now", now: channel.now() });
      };`;
    const worker = new Worker(URL.createObjectURL(new Blob([src], { type: "text/javascript" })), { type: "module" });
    const reply = (type) => new Promise((resolve) => { const on = ({ data }) => { if (data.type === type) { worker.removeEventListener("message", on); resolve(data); } }; worker.addEventListener("message", on); });
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
    const cw = new window.Clockwork({ ...config });
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) { worker.terminate(); return { init }; }
    const ch = cw.createOscChannel();
    const ready = reply("ready");
    worker.postMessage({ type: "channel", channel: ch.transferable }, ch.transferList);
    await ready;
    // the worker's reading and the page's engine clock as it arrives: the worker's is a message older, and a block
    const both = async () => { const got = reply("now"); worker.postMessage({ type: "now" }); const { now } = await got; return { worker: now, page: cw.clock.now() }; };
    await sleep(300);
    const running = [];
    for (let i = 0; i < 5; i++) { running.push(await both()); await sleep(60); }
    // suspended: the audio stands still, and so does the clock the worker reads
    await cw.suspend();
    await sleep(250);
    const s0 = await both();
    await sleep(500);
    const s1 = await both();
    await cw.resume();
    await sleep(300);
    const resumed = await both();
    // a reload: the engine made again, and its clock read by the same channel (SAB: the arena outlives the engine)
    let reloaded = null;
    if (mode === "sab") {
      const ok = await within(cw.reload(), 15000);
      await sleep(400);
      reloaded = { ok, ...(await both()) };
    }
    await cw.shutdown();
    worker.terminate();
    return { init, running, suspended: [s0, s1], resumed, reloaded };
  }, [clockworkConfig, within, clockworkMode]);
  expect(r.init.error ?? null).toBeNull();
  for (const { worker, page } of r.running) {
    expect(worker, "the worker read no clock").toBeGreaterThan(3.9e9);
    expect(Math.abs(page - worker), `the worker's clock ${worker} is not the page's ${page}`).toBeLessThan(0.05);
  }
  const [s0, s1] = r.suspended;
  expect(Math.abs(s1.worker - s0.worker), "the worker's clock moved while the audio was suspended").toBeLessThan(0.005);
  expect(Math.abs(r.resumed.page - r.resumed.worker), "after a resume the worker's clock is not the page's").toBeLessThan(0.05);
  expect(r.resumed.worker, "after a resume the worker's clock did not move on").toBeGreaterThan(s1.worker);
  if (r.reloaded) {
    expect(r.reloaded.ok).toEqual({ settled: true, value: true });
    expect(Math.abs(r.reloaded.page - r.reloaded.worker), "after a reload the worker's clock is not the new engine's").toBeLessThan(0.05);
  }
});

test("the transport stamp is a real time from boot, not the 1900 epoch", async ({ page, clockworkConfig }) => {
  // The worklet clock anchors the transport stamp into the clock state as it
  // binds, during init_memory; a region reset that ran after it wiped the
  // stamp to 0 (NTP 1900) on the web, where native had re-anchored later
  // (2026-10-16). The reply must carry the anchor, so a client's arithmetic
  // across it is not 126 years out.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const osc = window.Clockwork.osc;
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    const heard = [];
    cw.on("in:osc", ({ oscData }) => { try { heard.push(osc.decode(oscData)); } catch { /* a bundle */ } });
    cw.send("/clockwork/clock/transport/time/get");
    const deadline = performance.now() + 5000;
    const reply = () => heard.find((m) => m[0] === "/clockwork/clock/transport/time.reply");
    while (!reply() && performance.now() < deadline) await new Promise((res) => setTimeout(res, 20));
    const got = reply();
    await cw.destroy();
    return got ? Number(got[1]) : null;
  }, clockworkConfig);
  // NTP microseconds: anything after 2023 is above 3.9e15; 0 is the epoch.
  expect(r, "the stamp is anchored to wall time as the clock enters service").toBeGreaterThan(3.9e15);
});
