/*
 * OscChannel: channels opened from a transferable, as a worker opens one,
 * each reach the engine, however many are opened.
 *
 * A channel used to stand on a client slot, and there were eight: a page that
 * opened and closed workers ran out, and the next channel could not open at
 * all. A channel now writes the ingress ring itself (js/lib/ring.js) and holds
 * nothing but the shared memory, so there is no number to run out of.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("channels opened from their transferable each reach the engine, however many are opened", async ({ page, clockworkConfig, clockworkMode }) => {
  test.skip(clockworkMode === "postMessage", "a transferable carries the shared memory only in SAB mode");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const { OscChannel } = await import(`${location.origin}/js/lib/osc_channel.js`);
    const cw = new window.Clockwork({ ...config });
    await cw.init();

    // /clockwork/sync [i id] is answered /clockwork/synced [i id], so each
    // channel's message is heard back by its id.
    const heard = new Set();
    const decoder = new TextDecoder();
    cw.on("in:osc", ({ oscData }) => {
      if (decoder.decode(oscData.subarray(0, 17)) !== "/clockwork/synced") return;
      heard.add(new DataView(oscData.buffer, oscData.byteOffset, oscData.byteLength)
        .getInt32(oscData.byteLength - 4, false));
    });
    const sync = (id) => {
      const out = new Uint8Array(16 + 4 + 4);   // "/clockwork/sync" padded to 16, ",i\0\0", the int
      const text = (s, at) => { for (let i = 0; i < s.length; i++) out[at + i] = s.charCodeAt(i); };
      text("/clockwork/sync", 0);
      text(",i", 16);
      new DataView(out.buffer).setInt32(20, id, false);
      return out;
    };

    const { transferable } = cw.createOscChannel();
    const count = 20;   // more than the eight slots there used to be
    let sent = 0;
    for (let i = 0; i < count; i++) {
      const ch = await OscChannel.fromTransferable(transferable);
      if (ch.send(sync(1000 + i))) sent++;
      ch.close();
    }
    const deadline = performance.now() + 5000;
    while (heard.size < count && performance.now() < deadline) await new Promise((res) => setTimeout(res, 20));
    await cw.destroy();
    return { count, sent, heard: heard.size };
  }, clockworkConfig);
  expect(r.sent, "every channel took its message").toBe(r.count);
  expect(r.heard, "and the engine answered every one").toBe(r.count);
});
