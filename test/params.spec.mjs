// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * The parameter table in the browser (docs/PARAMS.md): the verbs on either
 * transport, answered by the worklet, and the region a page reads where it
 * shares the memory. The native cases are test_params.cpp; the two readers
 * agree on one fixture in params.test.mjs.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("the verbs define, list and set, and the table reads the same", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async ([config]) => {
    const { encodeDefine, KIND, FLAG, UNIT, TAPER } = await import(`${location.origin}/js/lib/params.js`);
    const cw = new window.Clockwork(config);
    await cw.init();
    const heard = [];
    cw.on("in:osc", ({ oscData }) => {
      try { heard.push(window.Clockwork.osc.decode(oscData)); } catch { /* a bundle */ }
    });
    const wait = async (address) => {
      for (let i = 0; i < 300; i++) {
        const m = heard.find((h) => h[0] === address);
        if (m) return m.slice(1);
        await new Promise((res) => setTimeout(res, 10));
      }
      return null;
    };
    cw.send("/clockwork/param/define", ...encodeDefine([
      { id: 8000, name: "drive", kind: KIND.CONTINUOUS, min: 0, max: 2, def: 1, flags: FLAG.AUTOMATABLE },
      { id: 8001, name: "Temp", fullName: "Coolant temperature", doc: "How hot it runs", kind: KIND.CONTINUOUS, min: -40, max: 125, def: 20,
        unit: UNIT.CUSTOM, unitText: "°C", flags: FLAG.READ_ONLY },
      { id: 8002, name: "Mode", kind: KIND.CHOICE, max: 2, def: 1, valueNames: ["Idle", "Run", "Fault"] },
      { id: 8003, name: "Speed", kind: KIND.CONTINUOUS, min: 100, max: 10000, def: 1000, taper: TAPER.LOG },
    ]));
    const defined = await wait("/clockwork/param/define.reply");
    cw.send("/clockwork/param/set", 8000, { type: "float", value: 5 });
    cw.send("/clockwork/param/list");
    const listed = await wait("/clockwork/param/list.reply");
    const rows = heard.filter((h) => h[0] === "/clockwork/param/entry");
    for (let i = 0; i < 100 && !rows.length; i++) {
      await new Promise((res) => setTimeout(res, 10));
      rows.push(...heard.filter((h) => h[0] === "/clockwork/param/entry"));
    }
    const row = rows.find((h) => h[1] === 8000)?.slice(1);

    // The memory read directly, in either mode: in postMessage mode, the snapshot's
    // copy, so give the next one time to carry the define and the set.
    const reader = cw.paramsReader();
    const seeDrive = async () => {
      for (let i = 0; i < 300; i++) {
        reader.refresh();
        const e = reader.find(8000);
        if (e && reader.value(e.slot) === 2) return e;
        await new Promise((res) => setTimeout(res, 10));
      }
      return null;
    };
    const drive = await seeDrive();
    const table = cw.params();
    const seen = {
      driveValue: drive ? reader.value(drive.slot) : null,
      generation: reader.generation,
      refreshAgain: reader.refresh(),
      temp: reader.find(8001),
      mode: reader.find(8002),
    };
    const c = await cw.paramConversions();
    const said = {
      temp: c.textOf(seen.temp, 20),
      mode: c.textOf(seen.mode, 1),
      modeBack: c.valueOfText(seen.mode, "fault"),
      speedMiddle: c.valueAt(reader.find(8003), 0.5),
    };
    const metrics = cw.getMetrics();
    await cw.shutdown();
    // The table is read and never announced: nothing was heard of the define or the set.
    const announced = heard.filter((h) => h[0] === "/clockwork/param/changed" || h[0] === "/clockwork/param/defined").length;
    return { defined, announced, listed, row, table, seen, said,
             metrics: { live: metrics.paramsLive, generation: metrics.paramsGeneration, moves: metrics.paramMoves } };
  }, [clockworkConfig]);

  expect(r.defined, "a define is answered").toBeTruthy();
  expect(r.announced, "a change is in the table, and announced to nobody").toBe(0);
  expect(r.listed?.[2], "one page").toBe(0);
  expect(r.row?.[1]).toBe("drive");
  // In both modes, shared memory or the snapshot's copy of it.
  expect(r.table, "the table is read directly").toBeTruthy();
  expect(r.table.torn).toBe(false);
  const drive = r.table.entries.find((e) => e.id === 8000);
  expect(drive.name).toBe("drive");
  expect(drive.value).toBe(2);
  expect(r.table.generation).toBe(r.defined[0]);
  expect(r.seen.driveValue).toBe(2);
  expect(r.seen.generation).toBe(r.defined[0]);
  expect(r.seen.refreshAgain, "nothing moved since").toBe(false);
  expect(r.seen.temp.fullName).toBe("Coolant temperature");
  expect(r.seen.temp.doc).toBe("How hot it runs");
  expect(r.seen.temp.unitText).toBe("°C");
  expect(r.seen.mode.valueNames).toEqual(["Idle", "Run", "Fault"]);
  // One implementation of what they mean.
  expect(r.said.temp).toBe("20 °C");
  expect(r.said.mode).toBe("Run");
  expect(r.said.modeBack).toBe(2);
  expect(Math.round(r.said.speedMiddle)).toBe(1000);
  // And the metrics say the same.
  expect(r.metrics.live).toBe(4);
  expect(r.metrics.generation).toBe(r.defined[0]);
  expect(r.metrics.moves).toBeGreaterThanOrEqual(1);
});
