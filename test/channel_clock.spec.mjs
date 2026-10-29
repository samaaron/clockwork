// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * The engine's clock in a worker: OscChannel#now.
 *
 * A worker cannot see the AudioContext. A time the page posts once a second and the worker counts on from goes
 * stale whenever the audio is away: the next run then starts late by the gap. The channel reads the audio thread's
 * own time — the sample clock it publishes (SAB), or its word down the channel's port (postMessage) — and this is
 * what that time must be: the page's engine clock, standing still with the audio, and following it back.
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
  // binds, during init_memory; a region reset run after it would wipe the
  // stamp to 0 (NTP 1900). The reply must carry the anchor, so a client's
  // arithmetic across it is not 126 years out.
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

test("after a reload onto the context the host supplied, a worker's channel reads the engine's clock moving on", async ({ page, clockworkConfig, clockworkMode }) => {
  // The host keeps its context across a reload (lifecycle.spec.mjs), so the context's time goes on and the NTP time
  // it started at is the same: what the host writes for it after the reload may be the very value already there. The
  // engine's clock comes back into service all the same, and the worker reads it moving, with the page's.
  test.skip(clockworkMode !== "sab", "a worker reads the engine's clock from the arena in SAB mode");
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
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
    const ctx = new AudioContext();
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) { worker.terminate(); return { init }; }
    const ch = cw.createOscChannel();
    const ready = reply("ready");
    worker.postMessage({ type: "channel", channel: ch.transferable }, ch.transferList);
    await ready;
    const both = async () => { const got = reply("now"); worker.postMessage({ type: "now" }); const { now } = await got; return { worker: now, page: cw.clock.now() }; };
    await sleep(300);
    let resumed = null;
    cw.on("reload:complete", (d) => { resumed = d?.resumed ?? null; });
    const ok = await within(cw.reload(), 15000);
    await sleep(400);
    const a = await both();
    await sleep(300);
    const b = await both();
    await cw.shutdown();
    await ctx.close();
    worker.terminate();
    return { init, ok, resumed, a, b };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  expect(r.ok).toEqual({ settled: true, value: true });
  expect(r.resumed).toBe(true);
  expect(r.b.worker - r.a.worker, "the worker's clock stood still after the reload").toBeGreaterThan(0.2);
  expect(Math.abs(r.b.page - r.b.worker), `the worker's clock ${r.b.worker} is not the page's ${r.b.page}`).toBeLessThan(0.05);
});
