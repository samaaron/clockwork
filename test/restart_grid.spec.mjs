// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * The restart grid's browser cells (docs/RESTARTS.md, "Where it is pinned").
 *
 * A reload is the browser's way of replacing the instance — over the same memory on the SAB transport, into new
 * memory on postMessage — and the model is the native one: what was waiting for the old instance is refused once,
 * naming the generation that closed; the new instance's first word on its ring is setup, naming the one opened; the
 * generation never goes back. The page is the reader, hearing the engine's words as they arrive (in:osc).
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

// A cell still red until a row of docs/RESTARTS.md's "Not yet" is done is marked so: it passes while it fails, and
// fails the day it passes, so the mark goes with the fix that turns it green.
test("restart grid: the page at a reload", async ({ page, clockworkConfig }) => {
  test.fail(true, "Not yet 1 and 2: a reload is not a rebuild");
  await boot(page);
  const r = await page.evaluate(async ([config, listenSrc]) => {
    const cw = new window.Clockwork(config);
    await cw.init();
    const heard = eval(listenSrc)(cw);
    const before = cw.getInfo().generation;

    cw.send("/dummy/ping");                 // answered by the old instance
    await cw.sync();
    await cw.suspend();                      // nothing taken from here
    cw.send("/dummy/events/get");            // the guest's, waiting
    cw.send("/clockwork/sync", 7);           // clockwork's own, waiting
    const reloaded = await cw.reload();
    const after = cw.getInfo().generation;

    cw.send("/dummy/reconfigured");          // the new instance's
    await cw.sync();
    await new Promise((res) => setTimeout(res, 200));
    await cw.shutdown();
    return { reloaded, before, after, heard };
  }, [clockworkConfig, listen]);

  const { heard, before, after } = r;
  const info = `the page heard:\n  | ${shown(heard)}`;
  expect(r.reloaded, info).toBe(true);
  expect(after, `the generation the reload opened (${info})`).toBe(before + 1);

  const setups = where(heard, "/clockwork/setup");
  expect(setups.length, `setup said once, by the engine (${info})`).toBe(1);
  const setup = setups[0];
  expect(heard[setup][3], info).toBe(after);
  expect(where(heard, "/dummy/pong")[0], info).toBeLessThan(setup);
  const reply = where(heard, "/dummy/reconfigured.reply");
  expect(reply.length, info).toBe(1);
  expect(reply[0], info).toBeGreaterThan(setup);
  const synced = heard.flatMap((m, i) => (m[0] === "/clockwork/synced" && m[1] === 7 ? [i] : []));
  expect(synced.length, `the waiting sync answered once (${info})`).toBe(1);
  expect(synced[0], info).toBeGreaterThan(setup);

  expect(where(heard, "/dummy/events").length, `the waiting message reached an instance (${info})`).toBe(0);
  const refused = refusalsOf(heard, "/dummy/events/get");
  expect(refused.length, `the waiting message refused once (${info})`).toBe(1);
  expect(refused[0][2], info).toBe("rebuilding");
  expect(refused[0][3], info).toBe(before);
});

test("restart grid: the page purging what waited while it was suspended", async ({ page, clockworkConfig }) => {
  // What resume() does after a stall (purge(), then the context started), with the purge made while the audio
  // thread is still stopped: resume() starts the context first, and whether a block takes what waited before the
  // purge arrives is then the browser's timing.
  test.fail(true, "Not yet 3: the page's purge drops what waited");
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
