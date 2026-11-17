/*
 * OscChannel: channels opened from a transferable, as a worker opens one,
 * each reach the engine, however many are opened.
 *
 * A channel writes the ingress ring itself (js/lib/ring.js) and holds nothing
 * but the shared memory: there is no per-client slot, so a page that opens
 * and closes workers has no count to run out of.
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
    const count = 20;   // enough to exhaust a small fixed slot table
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

/*
 * A message the ring has no room for now, or could never take, is refused with the word, as one sent while there is
 * no instance is: a worker's channel says why, and the client hears /clockwork/error, as a native transport's sender
 * does (ClockworkEngine::ingest). The audio is suspended so nothing drains and the ring fills.
 */
test("a send the ring has no room for is refused as ring full, and one larger than it always takes as too big", async ({ page, clockworkConfig, clockworkMode }) => {
  test.skip(clockworkMode === "postMessage", "the boundary is the ring writer, which only SAB mode has");
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const { OscChannel } = await import(`${location.origin}/js/lib/osc_channel.js`);
    const osc = window.Clockwork.osc;
    const cw = new window.Clockwork({ ...config });
    await cw.init();
    const heard = [];
    cw.on("in:osc", ({ oscData }) => { try { heard.push(osc.decode(oscData)); } catch { /* a bundle */ } });
    const { transferable } = cw.createOscChannel();
    const worker = await OscChannel.fromTransferable(transferable);
    const refused = [];
    worker.onRefused = (bytes, reason) => refused.push(reason);

    // An OSC message of exactly `total` bytes: "/x", a blob filling the rest.
    const messageOfBytes = (total) => {
      const m = new Uint8Array(total);
      m.set([0x2f, 0x78, 0, 0, 0x2c, 0x62, 0, 0]);
      new DataView(m.buffer).setUint32(8, total - 12, false);
      return m;
    };
    const ring = transferable.bufferConstants.IN_RING_SIZE;

    await cw.audioContext.suspend();
    let sent = 0;
    while (sent < 64 && worker.send(messageOfBytes(ring / 4))) sent++;
    // and the rest of it, with messages the size of the client's ping, so that does not fit either
    const ping = osc.encodeMessage("/dummy/ping", []);
    for (let i = 0; i < 100000 && worker.send(ping); i++);
    const fullReason = worker.lastRefusal;
    cw.send("/dummy/ping");                                          // the client's own door, the ring still full
    // Half the ring less a frame's 16-byte header is the most it always takes (ring.js ringMaxMessage): 4 bytes more.
    const tooBigSent = worker.send(messageOfBytes(ring / 2 - 16 + 4));
    const tooBigReason = worker.lastRefusal;
    await cw.audioContext.resume();

    const deadline = performance.now() + 5000;
    while (!heard.some((m) => m[0] === "/clockwork/error") && performance.now() < deadline)
      await new Promise((res) => setTimeout(res, 20));
    worker.close();
    await cw.destroy();
    return { sent, fullReason, tooBigSent, tooBigReason, refused, refusal: heard.find((m) => m[0] === "/clockwork/error")?.slice(0, 3) ?? null };
  }, clockworkConfig);
  expect(r.sent, "a few large messages filled the ring").toBeGreaterThan(0);
  expect(r.sent).toBeLessThan(64);
  expect(r.fullReason).toBe("ring full");
  expect(r.refusal, "the client heard the refusal with the address and the reason").toEqual(["/clockwork/error", "/dummy/ping", "ring full"]);
  expect(r.tooBigSent).toBe(false);
  expect(r.tooBigReason).toBe("too big");
  expect(r.refused.at(-1)).toBe("too big");
  expect(new Set(r.refused)).toEqual(new Set(["ring full", "too big"]));
});
