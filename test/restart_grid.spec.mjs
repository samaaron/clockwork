// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * The restart grid's browser cells (docs/RESTARTS.md, "Where it is pinned").
 *
 * A reload is audio coming back, and the model is the native one: the least that gets the engine back where it was.
 * Over the same memory (the SAB transport), with nothing changed that the instance was built for, it carries on as
 * native's does across a device restart — its generation, everything it holds, and what waited for it delivered to
 * it. Into new memory (postMessage, whose worklet took its memory with it) the instance is replaced: what was waiting
 * for the old one is refused once, naming the generation that closed; the new instance's first word on its ring is
 * setup, naming the one opened; the generation never goes back. The page is the reader, hearing the engine's words as
 * they arrive (in:osc).
 *
 * A message waits when the audio thread is not running: the context is suspended before it is sent. What waited
 * while the audio was away is stale, however the audio comes back: refused as such once (purge(), resume(), or a
 * reload over the same memory), as native's watchdog refuses what waited through a stall — nothing plays late. And
 * the engine's clock, as a client reads it, never goes back (test_restart_grid.cpp reads it on native).
 */
import { test, expect, boot } from "./fixtures.mjs";

// What the page heard, decoded, in order: [address, ...args].
const listen = `(cw) => {
  const heard = [];
  cw.on("in:osc", ({ oscData }) => {
    const m = window.Clockwork.osc.decode(oscData);
    if (Array.isArray(m)) heard.push(m.map((a) => (a instanceof Uint8Array ? "<blob>" : a)));
  });
  return heard;
}`;

const where = (heard, address) => heard.flatMap((m, i) => (m[0] === address ? [i] : []));
const refusalsOf = (heard, address) => heard.filter((m) => m[0] === "/clockwork/error" && m[1] === address);
const shown = (heard) => heard.map((m) => m.join(" ")).join("\n  | ");
// The engine's clock as a channel made before the event reads it, every millisecond until stop(): the most it went
// back from one reading to the next, and the most it was from the page's wall clock from a moment on (\`from\`, page ms).
const clockWatch = `(cw) => {
  const ch = cw.createOscChannel();
  const seen = [];   // [page ms, engine ntp] every reading
  let last = 0, back = 0, readings = 0, backAt = -1;
  const timer = setInterval(() => {
    const t = ch.now();
    seen.push([Math.round(performance.now()), t]);
    if (!t) return;
    readings += 1;
    if (last && t < last && last - t > back) { back = last - t; backAt = seen.length - 1; }
    last = t;
  }, 1);
  const wall = (ms) => (performance.timeOrigin + ms) / 1000 + 2208988800;
  return { stop: (from) => {
    clearInterval(timer);
    let offWall = 0;
    for (const [ms, t] of seen) if (ms >= from && t && Math.abs(t - wall(ms)) > Math.abs(offWall)) offWall = t - wall(ms);
    return { back, readings, offWall, around: backAt < 0 ? [] : seen.slice(Math.max(0, backAt - 4), backAt + 4) };
  } };
}`;

// Twice: on a context of Clockwork's own, closed and made again by the reload, and on one the host gave it, which
// stays open across the reload (Sonic Pi's), its time going on.
for (const hostContext of [false, true])
test(`restart grid: the page at a reload${hostContext ? " onto the context it was given" : ""}`, async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, listenSrc, clockWatchSrc, hostContext]) => {
    const ctx = hostContext ? new AudioContext() : null;
    const cw = new window.Clockwork({ ...config, ...(ctx ? { audioContext: ctx } : {}) });
    await cw.init();
    const heard = eval(listenSrc)(cw);
    const clock = eval(clockWatchSrc)(cw);
    const before = cw.getInfo().generation;
    let resumed = null, setupSaid = 0;
    cw.on("reload:complete", (d) => { resumed = d?.resumed ?? null; });
    cw.on("setup", () => { setupSaid += 1; });

    cw.send("/clockwork/asset/commit", 11, 0, 0, 64, 0, 0, 0);   // something the instance holds
    cw.send("/dummy/ping");                 // answered by the old instance
    await cw.sync();
    await cw.suspend();                      // nothing taken from here
    cw.send("/dummy/events/get");            // the guest's, waiting
    cw.send("/clockwork/sync", 7);           // clockwork's own, waiting
    await new Promise((res) => setTimeout(res, 2500));   // the audio away for seconds, as a tab's long in the background
    const reloaded = await cw.reload();
    const reloadedAt = performance.now();
    // A channel made after the reload: it hears the new worklet (one made before it keeps, in postMessage mode, a
    // port to the old one, and hears nothing more).
    const clockAfter = eval(clockWatchSrc)(cw);
    const after = cw.getInfo().generation;

    cw.send("/dummy/reconfigured");          // the instance's after
    cw.send("/dummy/asset/last");            // what it holds after
    await cw.sync();
    await new Promise((res) => setTimeout(res, 200));
    cw.send("/dummy/asset/release", 11);
    await cw.sync();
    const clockSeen = { ...clock.stop(reloadedAt + 200), offWall: clockAfter.stop(reloadedAt + 200).offWall };
    await cw.shutdown();
    if (ctx) await ctx.close();
    return { reloaded, before, after, heard, resumed, setupSaid, clockSeen, mode: config.mode };
  }, [clockworkConfig, listen, clockWatch, hostContext]);

  const { heard, before, after } = r;
  const info = `the page heard:\n  | ${shown(heard)}`;
  expect(r.reloaded, info).toBe(true);
  expect(where(heard, "/clockwork/asset/committed").length, info).toBe(1);
  expect(r.clockSeen.readings, "the clock was read through the reload").toBeGreaterThan(100);
  expect(r.clockSeen.back, `the engine's clock went back ${r.clockSeen.back} s across the reload; the readings around it (page ms, ntp): ${JSON.stringify(r.clockSeen.around)}`).toBe(0);
  // The worklet's audio starts at the reload, and its clock is anchored to the wall afresh, as native's is at a device
  // start — not converged towards it from where it stood when the audio went away (a clock that lags by seconds and
  // then runs fast plays everything scheduled against it late).
  expect(Math.abs(r.clockSeen.offWall), `the engine's clock was ${(r.clockSeen.offWall * 1000).toFixed(0)} ms from the wall clock after the reload`).toBeLessThan(0.1);

  if (r.mode === "sab") {
    // Over the same memory: the same instance, carrying on. Its generation, no setup said by the engine or the page,
    // and the asset it was handed still its own. The audio was away (the context suspended, then the worklet made
    // again): what waited for the guest reaches it when it is back — the engine drops nothing for a gap; the page's
    // purge() is for a host that wants it gone. Clockwork's own is answered whatever the guest is doing.
    expect(r.resumed, info).toBe(true);
    expect(after, `the generation carried on (${info})`).toBe(before);
    expect(where(heard, "/clockwork/setup").length, `no setup (${info})`).toBe(0);
    expect(r.setupSaid, info).toBe(0);
    for (const answer of ["/dummy/pong", "/dummy/reconfigured.reply"])
      expect(where(heard, answer).length, `${answer} heard once (${info})`).toBe(1);
    expect(where(heard, "/dummy/events").length, `what waited reached the instance, once (${info})`).toBe(1);
    expect(refusalsOf(heard, "/dummy/events/get").length, `nothing refused (${info})`).toBe(0);
    expect(heard.filter((m) => m[0] === "/clockwork/synced" && m[1] === 7).length, info).toBe(1);
    const last = heard.find((m) => m[0] === "/dummy/asset/last.reply");
    expect(last?.[1], `the asset still held (${info})`).toBe(11);
    return;
  }

  expect(r.resumed, info).toBe(false);
  expect(r.setupSaid, info).toBe(1);
  expect(after, `the generation the reload opened (${info})`).toBe(before + 1);

  const setups = where(heard, "/clockwork/setup");
  expect(setups.length, `setup said once, by the engine (${info})`).toBe(1);
  const setup = setups[0];
  expect(heard[setup][3], info).toBe(after);
  expect(where(heard, "/dummy/pong")[0], info).toBeLessThan(setup);
  const reply = where(heard, "/dummy/reconfigured.reply");
  expect(reply.length, info).toBe(1);
  expect(reply[0], info).toBeGreaterThan(setup);
  expect(where(heard, "/dummy/events").length, `the waiting message reached an instance (${info})`).toBe(0);
  const refused = refusalsOf(heard, "/dummy/events/get");
  expect(refused.length, `the waiting message refused once (${info})`).toBe(1);
  expect(refused[0][2], info).toBe("rebuilding");
  expect(refused[0][3], info).toBe(before);

  // Clockwork's own is answered once, whichever engine answers it — its answers belong to no instance — and a sync
  // after everything sent before it.
  const synced = heard.flatMap((m, i) => (m[0] === "/clockwork/synced" && m[1] === 7 ? [i] : []));
  expect(synced.length, `the waiting sync answered once (${info})`).toBe(1);
  expect(synced[0], info).toBeGreaterThan(heard.indexOf(refused[0]));
});

test("restart grid: what waited while the audio was away plays when it is back, late; what is for later plays at its time", async ({ page, clockworkConfig }) => {
  // A context suspended for seconds, then resumed with nothing purged (a demo page waking from idle, a tab coming
  // back): an immediate message that waited, and a bundle whose time passed meanwhile, are delivered at the first
  // block back, late by the gap — the engine drops nothing for a gap; a page that wants them gone calls purge()
  // first. A bundle that waited but is for after it plays at its time. And the engine's clock steps to the wall at
  // that block, never going back, rather than catching up over seconds.
  await boot(page);
  const r = await page.evaluate(async ([config, listenSrc, clockWatchSrc]) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    const heard = eval(listenSrc)(cw);
    const clock = eval(clockWatchSrc)(cw);
    const wall = () => (performance.timeOrigin + performance.now()) / 1000 + 2208988800;
    const bundleAt = (t) => cw.sendOSC(window.Clockwork.osc.encodeBundle(t, [["/dummy/events/get"]]));

    await cw.suspend();
    cw.send("/dummy/events/get");                      // immediate: waits, and is due
    bundleAt(wall() + 0.5);                            // due during the gap
    await new Promise((res) => setTimeout(res, 2500));
    bundleAt(wall() + 0.5);                            // for after the audio is back
    const resumedAt = performance.now();
    const resumed = await cw.resume();
    await new Promise((res) => setTimeout(res, 1200));   // the bundle for later fires, and its reply arrives
    const clockSeen = clock.stop(resumedAt + 200);
    await cw.shutdown();
    return { resumed, heard, clockSeen };
  }, [clockworkConfig, listen, clockWatch]);

  const { heard } = r;
  const info = `the page heard:\n  | ${shown(heard)}`;
  expect(r.resumed, info).toBe(true);
  expect(heard.filter((m) => m[0] === "/clockwork/error").length, `nothing refused (${info})`).toBe(0);
  expect(where(heard, "/dummy/events").length, `the immediate, the bundle that fell due and the bundle for later all played (${info})`).toBe(3);
  expect(r.clockSeen.back, `the engine's clock went back ${r.clockSeen.back} s; around it (page ms, ntp): ${JSON.stringify(r.clockSeen.around)}`).toBe(0);
  expect(Math.abs(r.clockSeen.offWall), `the engine's clock was ${(r.clockSeen.offWall * 1000).toFixed(0)} ms from the wall clock after the resume`).toBeLessThan(0.1);
});

test("restart grid: the page purging what waited while it was suspended", async ({ page, clockworkConfig }) => {
  // What resume() does after a stall (purge(), then the context started), with the purge made while the audio
  // thread is still stopped: resume() starts the context first, and whether a block takes what waited before the
  // purge arrives is then the browser's timing.
  await boot(page);
  const r = await page.evaluate(async ([config, listenSrc]) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    const heard = eval(listenSrc)(cw);
    const before = cw.getInfo().generation;

    await cw.suspend();
    cw.send("/dummy/events/get");
    cw.send("/clockwork/sync", 7);
    await cw.purge();
    const resumed = await cw.resume();
    const after = cw.getInfo().generation;

    cw.send("/dummy/reconfigured");
    await cw.sync();
    await new Promise((res) => setTimeout(res, 200));
    await cw.shutdown();
    return { resumed, before, after, heard };
  }, [clockworkConfig, listen]);

  const { heard, before, after } = r;
  const info = `the page heard:\n  | ${shown(heard)}`;
  expect(r.resumed, info).toBe(true);
  expect(after, info).toBe(before);
  expect(where(heard, "/clockwork/setup").length, info).toBe(0);
  expect(where(heard, "/dummy/reconfigured.reply").length, info).toBe(1);
  // The same instance: what waited is answered by it, or refused as stale — once, either way.
  const answered = where(heard, "/dummy/events").length;
  const refused = refusalsOf(heard, "/dummy/events/get");
  expect(answered + refused.length, `the waiting message answered once (${info})`).toBe(1);
  for (const m of refused) expect(m[3], info).toBe(before);
  const synced = heard.filter((m) => m[0] === "/clockwork/synced" && m[1] === 7);
  expect(synced.length, `the waiting sync answered once (${info})`).toBe(1);
});
