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

test("recover() moves onto a new context when the host's no longer renders, and stays on it when it does", async ({ page, clockworkConfig }) => {
  // iOS, after an interruption (a call, another app, the page switched away from): the host's context comes back
  // saying it is running and renders nothing. No worklet on it processes again, the old one or a reload's new one, and
  // a context made once the press has gone (a reload's, seconds in) starts suspended. recover() makes the replacement
  // in the press itself; the quick resume tries the context there is, and only if that fails does the reload move
  // onto the replacement. Played here by a host context that will not start again.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const made = [];
    const Real = window.AudioContext;
    window.AudioContext = class extends Real { constructor(...a) { super(...a); made.push(this); } };
    try {
      // the context comes back: the quick resume, on the host's context, and the spare let go of
      const host = new Real();
      const cw = new window.Clockwork({ ...config, audioContext: host });
      await cw.init();
      await cw.suspend();
      const quick = await within(cw.recover(), 15000);
      const stayed = { quick, onHost: cw.audioContext === host, running: cw.audioContext?.state, spares: made.map((c) => c.state) };
      await cw.shutdown();
      await host.close();
      made.length = 0;
      // the context does not come back: the reload, onto the replacement recover() made
      const dead = new Real();
      const cw2 = new window.Clockwork({ ...config, audioContext: dead });
      await cw2.init();
      await dead.suspend();
      dead.resume = () => Promise.resolve();   // never starts again
      const recovered = await within(cw2.recover(), 20000);
      const moved = { recovered, onHost: cw2.audioContext === dead, running: cw2.audioContext?.state ?? null, hostClosed: dead.state === "closed", made: made.length };
      // and a later reload stays on the engine's own, not back onto the host's dead one
      moved.reloaded = await within(cw2.reload(), 15000);
      moved.afterReloadOnHost = cw2.audioContext === dead;
      await cw2.shutdown();
      await dead.close();
      return { stayed, moved };
    } finally {
      window.AudioContext = Real;
    }
  }, [clockworkConfig, within]);
  expect(r.stayed.quick, "recover() on a context that comes back").toEqual({ settled: true, value: true });
  expect(r.stayed.onHost, "a quick resume moved off the host's context").toBe(true);
  expect(r.stayed.running).toBe("running");
  expect(r.stayed.spares.every((s) => s === "closed"), `a spare context was left open: ${r.stayed.spares}`).toBe(true);
  expect(r.moved.recovered, "recover() could not bring back a context that will not start").toEqual({ settled: true, value: true });
  expect(r.moved.onHost, "the reload stayed on the dead context").toBe(false);
  expect(r.moved.running).toBe("running");
  expect(r.moved.hostClosed, "the engine closed a context it did not make").toBe(false);
  expect(r.moved.reloaded).toEqual({ settled: true, value: true });
  expect(r.moved.afterReloadOnHost, "a later reload went back to the host's dead context").toBe(false);
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

test("init() returns with the context it started running, however long the browser takes to start it", async ({ page, clockworkConfig }) => {
  // init() starts the context synchronously (a press allows it only then) and must not say it is up before the
  // context is: a page that reads the context or the clock the moment init() returns finds them going. The browser
  // is made slow here, 300 ms to start, so the order is the same on every machine: an init that does not wait loses.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const ctx = new AudioContext();
    await ctx.suspend();
    const resume = ctx.resume.bind(ctx);
    ctx.resume = () => new Promise((r) => setTimeout(r, 300)).then(resume);
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    const init = await within(cw.init(), 12000);
    const out = { init, context: ctx.state, initialized: cw.initialized };
    if (init.settled) await cw.shutdown();
    await ctx.close();
    return out;
  }, [clockworkConfig, within]);
  expect(r.init.settled, "init() never settled").toBe(true);
  expect(r.init.error ?? null).toBeNull();
  expect(r.context, "init() returned before the context it started was running").toBe("running");
  expect(r.initialized).toBe(true);
});

test("init() does not wait for ever on a context the browser will not start", async ({ page, clockworkConfig }) => {
  // A browser that starts audio only inside a press (iOS) leaves resume() unanswered outside one. init() waits for
  // the context a bounded while and then returns, the context still suspended, for the host to start with a press.
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const ctx = new AudioContext();
    await ctx.suspend();
    ctx.resume = () => new Promise(() => {});   // never answered, as without a press
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    const init = await within(cw.init(), 8000);
    const out = { init, context: ctx.state, initialized: cw.initialized };
    if (init.settled) await cw.shutdown();
    await ctx.close();
    return out;
  }, [clockworkConfig, within]);
  expect(r.init.settled, "init() waited for ever on a context that will not start").toBe(true);
  expect(r.init.error ?? null).toBeNull();
  expect(r.context).toBe("suspended");
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

test("a reload into a new instance puts the host's state back before it says the engine is ready", async ({ page, clockworkConfig }) => {
  // 'setup' and 'ready' are where a host rebuilds on a new instance (its groups, its FX). Said before the client's own
  // state is back — a subclass's synthdefs and buffers — whatever the host builds refers to what is not there yet.
  // Onto a context at another rate, which the placeholder cannot take: a new instance whatever the transport.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const order = [];
    class Host extends window.Clockwork { async restoreClientState() { order.push("restore"); } }
    const cw = new Host(config);
    await cw.init();
    const before = cw.getInfo().generation;
    cw.on("setup", () => order.push("setup"));
    cw.on("ready", () => order.push("ready"));
    let resumed = null;
    cw.on("reload:complete", (d) => { resumed = d?.resumed ?? null; });
    const other = cw.audioContext.sampleRate === 44100 ? 48000 : 44100;
    await cw.reload({ audioContext: new AudioContext({ sampleRate: other }) });
    const out = { order, resumed, rate: cw.audioContext.sampleRate, other, opened: cw.getInfo().generation - before };
    await cw.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.rate).toBe(r.other);
  expect(r.resumed).toBe(false);
  expect(r.opened, "the generation a new instance opened").toBe(1);
  expect(r.order).toEqual(["restore", "setup", "ready"]);
});

test("a reload over the same memory with nothing changed carries the instance on: nothing put back, no setup", async ({ page, clockworkConfig, clockworkMode }) => {
  // Audio coming back does the least that gets the engine back where it was (docs/RESTARTS.md). The instance a host
  // built on is still there, with everything it held: putting it back would load it all twice, and 'setup' would
  // build the host's world on it twice.
  test.skip(clockworkMode !== "sab", "a reload keeps the memory only in SAB mode");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const order = [];
    class Host extends window.Clockwork { async restoreClientState() { order.push("restore"); } }
    const cw = new Host(config);
    await cw.init();
    const before = cw.getInfo().generation;
    cw.on("setup", () => order.push("setup"));
    cw.on("ready", () => order.push("ready"));
    let resumed = null;
    cw.on("reload:complete", (d) => { resumed = d?.resumed ?? null; });
    const reloaded = await cw.reload();
    const out = { order, resumed, reloaded, same: cw.getInfo().generation === before };
    await cw.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.reloaded).toBe(true);
  expect(r.resumed).toBe(true);
  expect(r.same, "the generation carried on").toBe(true);
  expect(r.order).toEqual(["ready"]);
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

test("the engine's state is native's, and every change of it is said", async ({ page, clockworkConfig }) => {
  // One answer to "what is the engine doing?", in the words ClockworkEngine uses (src/engine_state.h), in place of a
  // host keeping its own flags from half a dozen events. Suspending the audio is the context's business, not a state.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork(config);
    const seen = [cw.getEngineState()];
    cw.on("statechange", ({ state, reason }) => seen.push(`${state} (${reason})`));
    await cw.init();
    await cw.suspend();
    await new Promise((r) => setTimeout(r, 100));
    await cw.resume();
    await cw.reload();
    await cw.shutdown();
    return { seen, final: cw.getEngineState() };
  }, clockworkConfig);
  expect(r.seen).toEqual(["stopped", "booting (init)", "running (boot)", "restarting (reload)", "running (reload)", "stopped (shutdown)"]);
  expect(r.final).toBe("stopped");
});

test("a boot that fails leaves the engine in 'error' until it is asked again", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork({ ...config, workletUrl: "/dist/workers/no-such-worklet.js" });
    const seen = [];
    cw.on("statechange", ({ state, error }) => seen.push(error ? `${state}!` : state));
    await cw.init().catch(() => {});
    return { seen, final: cw.getEngineState() };
  }, clockworkConfig);
  expect(r.seen).toEqual(["booting", "error!"]);
  expect(r.final).toBe("error");
});

// A page's visibility, as a browser would change it: document.visibilityState is read-only, so it is redefined, then
// the event it comes with is sent
const setVisibility = `(v) => { Object.defineProperty(document, "visibilityState", { value: v, configurable: true }); Object.defineProperty(document, "hidden", { value: v === "hidden", configurable: true }); document.dispatchEvent(new Event("visibilitychange")); }`;

test("a page going away takes the engine with it: shut down, its own context closed", async ({ page, clockworkConfig }) => {
  // pagehide is the last word a page gets — the tab closed, the page left for another, or put in the back/forward
  // cache. An engine still running after it is what iOS keeps playing with no page at all, a worker still scheduling.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    const ctx = cw.audioContext;
    window.dispatchEvent(new PageTransitionEvent("pagehide", { persisted: true }));
    await new Promise((r) => setTimeout(r, 300));
    return { state: cw.getEngineState(), context: ctx.state };
  }, clockworkConfig);
  expect(r.state).toBe("stopped");
  expect(r.context, "the engine's context outlived the page").toBe("closed");
});

test("a host can keep the engine through pagehide", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork({ ...config, pageLifecycle: { pagehide: "none" } });
    await cw.init();
    window.dispatchEvent(new PageTransitionEvent("pagehide", { persisted: true }));
    await new Promise((r) => setTimeout(r, 300));
    const out = { state: cw.getEngineState(), context: cw.audioContext?.state };
    await cw.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.state).toBe("running");
  expect(r.context).toBe("running");
});

test("a hidden page keeps playing by default, and suspends if the host asks, resuming when shown again", async ({ page, clockworkConfig }) => {
  // Keeping on is right for a performer with the tab behind another; a phone, where the page hidden is a page the
  // system may freeze at any moment, is better suspended.
  await boot(page);
  const r = await page.evaluate(async ([config, setVisibilitySrc]) => {
    const setVisibility = eval(setVisibilitySrc);
    const wait = (ms) => new Promise((r) => setTimeout(r, ms));
    const keep = new window.Clockwork(config);
    await keep.init();
    setVisibility("hidden"); await wait(300);
    const kept = keep.audioContext.state;
    setVisibility("visible"); await wait(100);
    await keep.shutdown();
    const pause = new window.Clockwork({ ...config, pageLifecycle: { hidden: "suspend" } });
    await pause.init();
    setVisibility("hidden"); await wait(300);
    const hidden = pause.audioContext.state;
    setVisibility("visible"); await wait(800);
    const shown = pause.audioContext.state;
    await pause.shutdown();
    return { kept, hidden, shown };
  }, [clockworkConfig, setVisibility]);
  expect(r.kept).toBe("running");
  expect(r.hidden).toBe("suspended");
  expect(r.shown).toBe("running");
});

test("a shut-down engine listens to the page no more", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, setVisibilitySrc]) => {
    const setVisibility = eval(setVisibilitySrc);
    const cw = new window.Clockwork({ ...config, pageLifecycle: { hidden: "suspend" } });
    await cw.init();
    await cw.shutdown();
    const seen = [];
    cw.on("statechange", ({ state }) => seen.push(state));
    for (const m of ["suspend", "resume", "shutdown"]) { const f = cw[m].bind(cw); cw[m] = (...a) => { seen.push(m); return f(...a); }; }
    setVisibility("hidden"); setVisibility("visible");
    window.dispatchEvent(new PageTransitionEvent("pagehide", { persisted: false }));
    await new Promise((r) => setTimeout(r, 300));
    return seen;
  }, [clockworkConfig, setVisibility]);
  expect(r).toEqual([]);
});

test("a 'setup' listener that fails is said on 'error', not swallowed", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork(config);
    const errors = [];
    cw.on("error", (e) => errors.push(String(e?.message ?? e)));
    cw.on("setup", () => { throw new Error("no such group"); });
    await cw.init();
    const out = { errors, state: cw.getEngineState() };
    await cw.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.errors.some((m) => m.includes("setup") && m.includes("no such group")), JSON.stringify(r.errors)).toBe(true);
  expect(r.state, "the engine itself is up").toBe("running");
});

test("after a reload onto the context the host supplied, one audio thread renders the engine, not two", async ({ page, clockworkConfig, clockworkMode }) => {
  // A reload makes a new worklet. The old one's node is let go, but on a context that stays open (the host's, kept
  // across the reload) its processor is still called while it asks to be, and one still rendering is a second audio
  // thread in the engine: a SAB reload's carried-on instance driven twice a block — scsynth reads a bus written in
  // the other thread's block as silence — and the in ring with two readers. The engine renders a block per render
  // quantum of the context, and no more. Counted where the page reads the count live (SAB); postMessage's snapshots
  // carry it late, and there the old worklet renders memory of its own.
  test.skip(clockworkMode !== "sab", "the engine's block count is read live in SAB mode");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const ctx = new AudioContext();
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    await cw.init();
    const rate = async () => {
      const n = () => cw.getMetrics().engineProcessCount, a = n(), t = performance.now();
      await new Promise((res) => setTimeout(res, 1500));
      return (n() - a) / ((performance.now() - t) / 1000);
    };
    const before = await rate();
    const reloaded = await cw.reload();
    const after = await rate();
    const quanta = ctx.sampleRate / 128;
    await cw.shutdown();
    await ctx.close();
    return { reloaded, before, after, quanta };
  }, clockworkConfig);
  expect(r.reloaded).toBe(true);
  expect(r.before / r.quanta, `before the reload: ${r.before.toFixed(0)} blocks a second at ${r.quanta} quanta`).toBeLessThan(1.2);
  expect(r.after / r.quanta, `after the reload: ${r.after.toFixed(0)} blocks a second at ${r.quanta} quanta`).toBeLessThan(1.2);
});

test("after a shutdown on the context the host supplied, the engine's audio thread renders no more", async ({ page, clockworkConfig, clockworkMode }) => {
  // The host's context outlives the engine it was given to, and a worklet still rendering on it once the engine has
  // shut down is an audio thread spent on nothing, for as long as the page lives. Counted where the page reads the
  // count live (SAB): the old memory's block count stands still once the engine is down.
  test.skip(clockworkMode !== "sab", "the engine's block count is read live in SAB mode");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const ctx = new AudioContext();
    const cw = new window.Clockwork({ ...config, audioContext: ctx });
    await cw.init();
    const count = () => cw.getMetrics().engineProcessCount;
    const sleep = (ms) => new Promise((res) => setTimeout(res, ms));
    const a = count();
    await sleep(500);
    const running = count() - a;
    await cw.shutdown();
    await sleep(100);   // a block already under way finishes
    const b = count();
    await sleep(1000);
    const after = count() - b;
    await ctx.close();
    return { running, after };
  }, clockworkConfig);
  expect(r.running, "the engine rendered before the shutdown").toBeGreaterThan(50);
  expect(r.after, "blocks rendered in the second after the shutdown").toBe(0);
});
