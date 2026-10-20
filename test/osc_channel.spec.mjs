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

/*
 * docs/RESTARTS.md at the browser's boundary: while the engine has no guest
 * instance, a message for the guest is refused where it is written, and the
 * client hears it as it hears every refusal — /clockwork/error on the same
 * path as the engine's replies — while clockwork's own goes through. The
 * word is the engine's (the arena's generation word, odd from a teardown
 * until the next build); the web build has no device to swap, so here it is
 * turned odd by hand.
 */
test("while there is no instance the client's send is refused with the word, a worker's channel says why, and clockwork's own goes through", async ({ page, clockworkConfig, clockworkMode }) => {
  test.skip(clockworkMode === "postMessage", "the boundary is the ring writer, which only SAB mode has");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const { OscChannel } = await import(`${location.origin}/js/lib/osc_channel.js`);
    const { generationView } = await import(`${location.origin}/js/lib/arena.js`);
    const osc = window.Clockwork.osc;
    const cw = new window.Clockwork({ ...config });
    await cw.init();

    const heard = [];
    cw.on("in:osc", ({ oscData }) => { try { heard.push(osc.decode(oscData)); } catch { /* a bundle */ } });
    const { transferable } = cw.createOscChannel();
    const word = generationView(transferable.sharedBuffer, transferable.arenaBase);
    const worker = await OscChannel.fromTransferable(transferable);
    const refused = [];
    worker.onRefused = (bytes, reason) => refused.push(reason);

    Atomics.or(word, 0, 1);                      // odd: no instance, as at a teardown
    cw.send("/dummy/ping");                      // the client's own door
    const workerSent = worker.send(osc.encodeMessage("/dummy/ping", []));
    const workerReason = worker.lastRefusal;
    cw.send("/clockwork/sync", 4242);            // clockwork's own: written, answered once it renders
    Atomics.and(word, 0, ~1);
    cw.send("/dummy/ping");                      // an instance again: delivered

    const deadline = performance.now() + 5000;
    const got = (addr) => heard.some((m) => m[0] === addr);
    while (!(got("/clockwork/synced") && got("/dummy/pong")) && performance.now() < deadline)
      await new Promise((res) => setTimeout(res, 20));
    worker.close();
    await cw.destroy();
    return {
      refusal: heard.find((m) => m[0] === "/clockwork/error") ?? null,
      pongs: heard.filter((m) => m[0] === "/dummy/pong").length,
      synced: got("/clockwork/synced"),
      workerSent, workerReason, refused,
    };
  }, clockworkConfig);
  // ... and the generation it was refused in, read off the arena: the flag
  // was raised by hand, so the word still names the first instance.
  expect(r.refusal, "the client heard the refusal with the address, the reason and the generation").toEqual(["/clockwork/error", "/dummy/ping", "no instance", 1]);
  expect(r.pongs, "only the ping sent once there was an instance was answered").toBe(1);
  expect(r.synced, "clockwork's own went through while there was no instance").toBe(true);
  expect(r.workerSent).toBe(false);
  expect(r.workerReason).toBe("no instance");
  expect(r.refused).toEqual(["no instance"]);
});
