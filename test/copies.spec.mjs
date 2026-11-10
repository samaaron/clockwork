// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/**
 * What a page draws reads the same whether it can see the engine's memory
 * (SAB) or is handed copies of it (postMessage: the worklet's, made by
 * clockwork_client_copy). The device scope is written by the tick on every
 * host whatever the guest does, so a page draws the master mix from it: its
 * window is there in both modes, of the length asked, and moves on as the
 * engine renders, and after the engine is made again (a reload). The
 * metrics come the same way.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("the device scope and the metrics read alike with and without shared memory", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    const until = async (read) => {
      for (let i = 0; i < 200; i++) {
        const got = read();
        if (got) return got;
        await new Promise((res) => setTimeout(res, 20));
      }
      return null;
    };
    const first = await until(() => cw.getDeviceScope(0, 1024));
    await new Promise((res) => setTimeout(res, 200));
    const later = await until(() => {
      const w = cw.getDeviceScope(0, 1024);
      return w && first && w.writePosition > first.writePosition ? w : null;
    });
    const processed = await until(() => cw.getMetricsArray()?.[0] || null);
    // a reload makes the engine again: its scope is read again, a new worklet told what the page reads
    await cw.reload();
    const before = cw.getDeviceScope(0, 1024)?.writePosition ?? 0n;
    const again = await until(() => {
      const w = cw.getDeviceScope(0, 1024);
      return w && w.writePosition !== before ? w : null;
    });
    const out = {
      first: first && { frames: first.frames, channels: first.channels, samples: first.interleaved.length },
      moved: !!later,
      processed,
      afterReload: !!again,
      noSuchSlot: cw.getDeviceScope(7, 64),
    };
    await cw.destroy();
    return out;
  }, clockworkConfig);
  expect(r.first, "the output's window").toEqual({ frames: 1024, channels: 2, samples: 2048 });
  expect(r.moved, "the window moves on as the engine renders").toBe(true);
  expect(r.processed, "the engine's blocks, counted").toBeGreaterThan(0);
  expect(r.afterReload, "read again after the engine is made again").toBe(true);
  expect(r.noSuchSlot, "a scope slot the device has not got").toBe(null);
});
