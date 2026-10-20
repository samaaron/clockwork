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
 * A message waits when the audio thread is not running: the context is suspended before it is sent. What waits
 * through a stall and is thrown away as stale (purge(), resume()) is refused as such, as native's wake refuses it.
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

test("restart grid: the page at a reload", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, listenSrc]) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    const heard = eval(listenSrc)(cw);
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
    const reloaded = await cw.reload();
    const after = cw.getInfo().generation;

    cw.send("/dummy/reconfigured");          // the instance's after
    cw.send("/dummy/asset/last");            // what it holds after
    await cw.sync();
    await new Promise((res) => setTimeout(res, 200));
    cw.send("/dummy/asset/release", 11);
    await cw.sync();
    await cw.shutdown();
    return { reloaded, before, after, heard, resumed, setupSaid, mode: config.mode };
  }, [clockworkConfig, listen]);

  const { heard, before, after } = r;
  const info = `the page heard:\n  | ${shown(heard)}`;
  expect(r.reloaded, info).toBe(true);
  expect(where(heard, "/clockwork/asset/committed").length, info).toBe(1);

  if (r.mode === "sab") {
    // Over the same memory: the same instance, carrying on. Its generation, no setup said by the engine or the page,
    // everything that waited delivered to it and nothing refused, and the asset it was handed still its own.
    expect(r.resumed, info).toBe(true);
    expect(after, `the generation carried on (${info})`).toBe(before);
    expect(where(heard, "/clockwork/setup").length, `no setup (${info})`).toBe(0);
    expect(r.setupSaid, info).toBe(0);
    expect(where(heard, "/clockwork/error").length, `nothing refused (${info})`).toBe(0);
    for (const answer of ["/dummy/pong", "/dummy/events", "/dummy/reconfigured.reply"])
      expect(where(heard, answer).length, `${answer} heard once (${info})`).toBe(1);
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
