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

test("reload() puts the host's state back before it says the engine is ready", async ({ page, clockworkConfig }) => {
  // 'setup' and 'ready' are where a host rebuilds on the engine (its groups, its FX). Said before the client's own
  // state is back — a subclass's synthdefs and buffers — whatever the host builds refers to what is not there yet.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const order = [];
    class Host extends window.Clockwork { async restoreClientState() { order.push("restore"); } }
    const cw = new Host(config);
    await cw.init();
    cw.on("setup", () => order.push("setup"));
    cw.on("ready", () => order.push("ready"));
    await cw.reload();
    await cw.shutdown();
    return order;
  }, clockworkConfig);
  expect(r).toEqual(["restore", "setup", "ready"]);
});

test("reloads asked for at once are one reload, and each caller hears how it went", async ({ page, clockworkConfig }) => {
  // Two presses, a tap and a key, a visibility change and a press: recovery is asked for from several places at
  // the same moment. Two teardowns racing two builds leave nothing working.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork(config);
    await cw.init();
    let starts = 0;
    cw.on("reload:start", () => starts++);
    const both = await within(Promise.all([cw.reload(), cw.reload()]), 20000);
    const out = { both, starts, initialized: cw.initialized, context: cw.audioContext?.state ?? null };
    await cw.shutdown();
    return out;
  }, [clockworkConfig, within]);
  expect(r.both).toEqual({ settled: true, value: [true, true] });
  expect(r.starts, "two reloads ran").toBe(1);
  expect(r.initialized).toBe(true);
  expect(r.context).toBe("running");
});

test("a boot that fails leaves nothing behind: its context closed, and the client ready to be asked again", async ({ page, clockworkConfig }) => {
  // A worklet that will not load: init() must say so, and not leave a context running (on a phone, holding the audio
  // session) or a client that believes it is still starting.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork({ ...config, workletUrl: "/dist/workers/no-such-worklet.js" });
    const init = cw.init();
    const ctx = cw.audioContext;   // made synchronously, in init()'s first tick
    const settled = await within(init, 12000);
    await new Promise((r) => setTimeout(r, 100));
    return { settled, context: ctx?.state ?? null, initialized: cw.initialized, initializing: cw.initializing, engineContext: cw.audioContext ? cw.audioContext.state : null };
  }, [clockworkConfig, within]);
  expect(r.settled.settled, "init() never settled").toBe(true);
  expect(r.settled.error, "init() did not report the failure").toBeTruthy();
  expect(r.context, "the failed boot left its context open").toBe("closed");
  expect(r.initialized).toBe(false);
  expect(r.initializing, "the client still thinks it is starting").toBe(false);
  expect(r.engineContext).toBeNull();
});

test("a reload that fails says so, answers false, and leaves nothing half-built", async ({ page, clockworkConfig }) => {
  // recover() ends in reload(): a reload that throws past it, or leaves a context open and the client half up, turns
  // one failure into an audio stack that can never be asked to start again.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork(config);
    await cw.init();
    const failed = [];
    cw.on("reload:failed", (e) => failed.push(String(e?.error?.message ?? e?.error ?? e)));
    const addModule = AudioWorklet.prototype.addModule;
    AudioWorklet.prototype.addModule = () => Promise.reject(new Error("the worklet has gone"));
    const made = [];
    const Real = window.AudioContext;
    window.AudioContext = class extends Real { constructor(...a) { super(...a); made.push(this); } };
    const reload = await within(cw.reload(), 15000);
    window.AudioContext = Real;
    AudioWorklet.prototype.addModule = addModule;
    const out = { reload, failed, initialized: cw.initialized, made: made.map((c) => c.state) };
    // and asked again, it comes back
    out.reset = await within(cw.reset().then(() => cw.initialized), 15000);
    await cw.shutdown();
    return out;
  }, [clockworkConfig, within]);
  expect(r.reload, "reload() threw or hung rather than answering").toEqual({ settled: true, value: false });
  expect(r.failed.length, "no reload:failed was said").toBe(1);
  expect(r.initialized).toBe(false);
  expect(r.made.every((s) => s === "closed"), `a context the failed reload made is still open: ${r.made}`).toBe(true);
  expect(r.reset, "the client could not be started again after a failed reload").toEqual({ settled: true, value: true });
});

test("shutdown() while a boot is under way stops it, and the boot settles", async ({ page, clockworkConfig }) => {
  // A page going away mid-boot (a phone locking, a tab closing): shutdown() must win, and init() must not carry on
  // building on what shutdown() took away.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork(config);
    const init = cw.init();
    const ctx = cw.audioContext;
    await cw.shutdown();
    const settled = await within(init, 12000);
    await new Promise((r) => setTimeout(r, 200));
    return { settled, context: ctx?.state ?? null, initialized: cw.initialized, initializing: cw.initializing };
  }, [clockworkConfig, within]);
  expect(r.settled.settled, "init() never settled after shutdown()").toBe(true);
  expect(r.initialized, "the client came up after it was shut down").toBe(false);
  expect(r.initializing).toBe(false);
  expect(r.context, "the context outlived shutdown()").toBe("closed");
});

test("shutdown() settles what is waiting on the engine at once, rather than seconds later", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork(config);
    await cw.init();
    const pending = cw.sync(undefined, 10000);
    pending.catch(() => {});
    await cw.shutdown();
    return await within(pending, 1500);
  }, [clockworkConfig, within]);
  expect(r.settled, "a sync() pending at shutdown() was left to time out").toBe(true);
});

test("a 'setup' listener that fails is said on 'error', not swallowed", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork(config);
    const errors = [];
    cw.on("error", (e) => errors.push(String(e?.message ?? e)));
    cw.on("setup", () => { throw new Error("no such group"); });
    await cw.init();
    const out = { errors, initialized: cw.initialized };
    await cw.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.errors.some((m) => m.includes("setup") && m.includes("no such group")), JSON.stringify(r.errors)).toBe(true);
  expect(r.initialized, "the engine itself is up").toBe(true);
});
