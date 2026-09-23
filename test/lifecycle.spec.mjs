// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * The client's lifecycle: starting, stopping, suspending, resuming, recovering, reloading.
 *
 * boot.spec.mjs starts clockwork and shuts it down. Nothing tested what happens in between, and every one of these
 * was found by a host instead — an app on a phone whose audio came back dead after an interruption, or never
 * started. The guest is dsp/dummy, as in boot.spec.mjs: the host is what is tested.
 */
import { test, expect, boot } from "./fixtures.mjs";

// A promise raced against a deadline: a lifecycle call that never settles is a bug of its own, and must fail the
// test rather than time the whole run out
const within = `(p, ms) => Promise.race([p.then((v) => ({ settled: true, value: v }), (e) => ({ settled: true, error: String(e?.message ?? e) })), new Promise((r) => setTimeout(() => r({ settled: false }), ms))])`;

test("a context the host supplies survives a reload, and the engine reloads onto it", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const ctx = new AudioContext();
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    await cw.init();
    const reload = await within(cw.reload(), 15000);
    const out = { reload, hostContext: ctx.state, engineContext: cw.audioContext?.state ?? null, sameContext: cw.audioContext === ctx, initialized: cw.initialized };
    await cw.shutdown();
    out.hostContextAfterShutdown = ctx.state;
    await ctx.close();
    return out;
  }, [clockworkConfig, within]);
  expect(r.reload, "reload() failed or hung on a supplied context").toEqual({ settled: true, value: true });
  expect(r.hostContext, "the engine closed a context it did not make").not.toBe("closed");
  expect(r.sameContext, "the engine swapped out the host's context").toBe(true);
  expect(r.engineContext).toBe("running");
  expect(r.initialized).toBe(true);
  // the host made it, so the host closes it: shutdown lets go of it, and leaves it open
  expect(r.hostContextAfterShutdown, "shutdown() closed the host's context").not.toBe("closed");
});

test("init() starts a context that begins suspended, rather than hanging on it", async ({ page, clockworkConfig }) => {
  // What a browser does to a context made outside a gesture (iOS, autoplay rules): it is born suspended. The engine
  // is asked to start inside a press, so starting the context is its job — and waiting on a clock that can never
  // move, forever, is the one thing it must not do.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const ctx = new AudioContext();
    await ctx.suspend();
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    const init = await within(cw.init(), 12000);
    const out = { init, context: ctx.state, initialized: cw.initialized };
    if (init.settled) await cw.shutdown();
    await ctx.close();
    return out;
  }, [clockworkConfig, within]);
  expect(r.init.settled, "init() never settled on a suspended context").toBe(true);
  expect(r.init.error ?? null).toBeNull();
  expect(r.context).toBe("running");
  expect(r.initialized).toBe(true);
});

test("resume() on an engine that is already running changes nothing, and drops nothing scheduled", async ({ page, clockworkConfig }) => {
  // resume() is what a host calls when it is not sure — back to the tab, a press after a stall. On an engine that
  // never stopped it must be free: a purge here throws away every bundle scheduled ahead, and the music with it.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    let purged = 0;
    const purge = cw.purge.bind(cw);
    cw.purge = (...a) => { purged++; return purge(...a); };
    const resumed = await cw.resume();
    await cw.shutdown();
    return { resumed, purged };
  }, clockworkConfig);
  expect(r.resumed).toBe(true);
  expect(r.purged, "resume() purged a running engine's schedule").toBe(0);
});
