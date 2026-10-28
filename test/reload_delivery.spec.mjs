// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * Nothing sent around a reload is lost. The client's own comment says the
 * first message or two after a reload could go missing on the reused-SAB
 * path, and #syncWithRetry stands guard against it; a product's restore step
 * (restoreClientState) sends before that sync and waits for nothing. These
 * cases send bursts at both moments, the instant reload() returns and inside
 * the restore step, and count every answer. Written 2026-10-28 to reproduce a
 * sample missing after a reload (SuperSonic's lifecycle_buffers case, once,
 * 2026-10-08): 150 reloads a case on each transport lost nothing, so this is
 * the guard that says so on every push.
 */
import { test, expect, boot } from "./fixtures.mjs";
const within = `(p, ms) => Promise.race([p.then((v) => ({ settled: true, value: v }), (e) => ({ settled: true, error: String(e?.message ?? e) })), new Promise((r) => setTimeout(() => r({ settled: false }), ms))])`;

test("every message sent the moment reload() returns is answered", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork(config);
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) return { init };
    const decoder = new TextDecoder();
    let pongs = 0;
    cw.on("in:osc", ({ oscData }) => { if (decoder.decode(oscData.subarray(0, 12)).startsWith("/dummy/pong")) pongs++; });
    const rounds = [];
    for (let round = 0; round < 5; round++) {
      const reloaded = await cw.reload();
      const pongsBefore = pongs;
      // Sent the instant reload() has returned: twenty syncs and twenty pings, nothing awaited between them.
      const syncs = [];
      for (let i = 0; i < 20; i++) { syncs.push(within(cw.sync(undefined, 3000), 3500)); cw.send("/dummy/ping"); }
      const results = await Promise.all(syncs);
      await new Promise((res) => setTimeout(res, 500));
      rounds.push({ reloaded, syncsAnswered: results.filter((x) => x.settled && !x.error).length, pongs: pongs - pongsBefore });
    }
    await cw.shutdown();
    return { init, rounds };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  for (const [i, round] of r.rounds.entries()) {
    expect(round.reloaded, `reload ${i} failed`).toBe(true);
    expect(round.syncsAnswered, `round ${i}: syncs answered of 20 (${JSON.stringify(round)})`).toBe(20);
    expect(round.pongs, `round ${i}: pongs of 20 (${JSON.stringify(round)})`).toBe(20);
  }
});

test("every message sent during the restore step of a rebuild is answered", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const cw = new window.Clockwork(config);
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) return { init };
    const decoder = new TextDecoder();
    let pongs = 0;
    cw.on("in:osc", ({ oscData }) => { if (decoder.decode(oscData.subarray(0, 12)).startsWith("/dummy/pong")) pongs++; });
    let restores = 0;
    cw.restoreClientState = async () => { restores++; for (let i = 0; i < 20; i++) cw.send("/dummy/ping"); };
    const rounds = [];
    for (let round = 0; round < 5; round++) {
      const pongsBefore = pongs, restoresBefore = restores;
      const reloaded = await cw.reload();
      await new Promise((res) => setTimeout(res, 800));
      rounds.push({ reloaded, restored: restores - restoresBefore, pongs: pongs - pongsBefore });
    }
    await cw.shutdown();
    return { init, rounds, mode: config.mode };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  for (const [i, round] of r.rounds.entries()) {
    expect(round.reloaded, `reload ${i} failed`).toBe(true);
    if (round.restored === 0) continue;   // the instance carried on (SAB): nothing was restored, nothing to lose
    expect(round.pongs, `round ${i}: pongs of 20 sent in restoreClientState (${JSON.stringify(round)}, ${r.mode})`).toBe(20);
  }
});
