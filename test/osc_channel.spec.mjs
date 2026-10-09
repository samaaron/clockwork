/*
 * OscChannel: a channel opened from its transferable (as a worker opens one)
 * claims a client slot of its own, and closing the channel gives it back.
 *
 * It did not: close() released nothing in SAB mode, so every worker that came
 * and went kept a slot, and once they were all taken the next channel could
 * not open at all ("no client slot free").
 */
import { test, expect, boot } from "./fixtures.mjs";

test("closing a channel opened from its transferable frees its client slot", async ({ page, clockworkConfig, clockworkMode }) => {
  test.skip(clockworkMode === "postMessage", "client slots are the SharedArrayBuffer transport's");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const { OscChannel } = await import(`${location.origin}/js/lib/osc_channel.js`);
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    const { transferable } = cw.createOscChannel();
    const slots = transferable.bufferConstants.CLIENT_SLOT_COUNT;
    let opened = 0, error = null;
    try {
      // Twice as many as there are slots: each must be given back to be reused.
      for (let i = 0; i < slots * 2; i++) {
        const ch = await OscChannel.fromTransferable(transferable);
        ch.close();
        opened++;
      }
    } catch (e) {
      error = String(e?.message ?? e);
    }
    await cw.destroy();
    return { slots, opened, error };
  }, clockworkConfig);
  expect(r.error).toBeNull();
  expect(r.opened).toBe(r.slots * 2);
});
