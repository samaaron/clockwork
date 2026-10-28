// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * A host's own OSC-in reader (oscInEndpoint) across reloads.
 *
 * How Sonic Pi runs it: in a worker that is also something else, on a MessagePort, waiting without blocking. A reload
 * stops the reader, hands it a new ring and starts it again — and the loop from before, parked in Atomics.waitAsync,
 * woke with the next frame, found the reader running again and read on beside the new one. One more loop per reload.
 */
import { test, expect, boot } from "./fixtures.mjs";

// A promise raced against a deadline: a call that never settles must fail the test rather than time the run out
const within = `(p, ms) => Promise.race([p.then((v) => ({ settled: true, value: v }), (e) => ({ settled: true, error: String(e?.message ?? e) })), new Promise((r) => setTimeout(() => r({ settled: false }), ms))])`;

test("a reader the host runs on a port of its own, yielding between frames, hears each reply once across reloads", async ({ page, clockworkConfig, clockworkMode }) => {
  // How a host embeds the reader in a worker that is also something else (Sonic Pi's runtime): runOscInPump on a
  // MessagePort, non-blocking. Each reload stopped it and started it again while its loop from before was parked in
  // Atomics.waitAsync — and that loop woke with the next frame and read on, beside the new one.
  test.skip(clockworkMode !== "sab", "a host's reader is the SAB transport's");
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const src = `
      globalThis.__DEV__ = false;
      self.onmessage = async ({ data }) => {
        const { runOscInPump } = await import("${location.origin}/js/lib/osc_in_pump.js");
        const port = data.port;
        runOscInPump({ name: "HostReader", endpoint: port, onFrames: (messages) => port.postMessage({ type: "messages", messages }) });
      };`;
    const host = new Worker(URL.createObjectURL(new Blob([src], { type: "text/javascript" })), { type: "module" });
    const channel = new MessageChannel();
    host.postMessage({ port: channel.port2 }, [channel.port2]);
    const cw = new window.Clockwork({ ...config, oscInEndpoint: channel.port1 });
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) { host.terminate(); return { init }; }
    // several reloads: a reader loop, a ring position or a write lock left over from any of them shows up here
    const reloads = 10;
    for (let i = 0; i < reloads; i++) if (!(await cw.reload())) { host.terminate(); return { init, reloadFailed: i }; }
    let synced = 0;
    const decoder = new TextDecoder();
    cw.on("in:osc", ({ oscData }) => { if (decoder.decode(oscData.subarray(0, 20)).startsWith("/clockwork/synced")) synced++; });
    await within(cw.sync(), 5000);
    await new Promise((r) => setTimeout(r, 300));
    await cw.shutdown();
    host.terminate();
    return { init, synced };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  expect(r.reloadFailed ?? null, "a reload failed").toBeNull();
  expect(r.synced, "one reply was heard more than once: a reader loop from before a reload is still reading").toBe(1);
});

test("a reader the host runs on a port of its own has one loop reading, however many times it is stopped and started", async ({ page, clockworkConfig, clockworkMode }) => {
  // Counted from inside: how many loops are inside their wait, and how many reads a loop made after it was stopped
  // or replaced. A loop that was stopped or replaced leaves at its next wake without reading, and a reply to a sync
  // is a frame that wakes every loop there is, so after a sync there is one loop and no stale read, or there is a
  // leak. Both counts are exact; nothing here is a timer.
  test.skip(clockworkMode !== "sab", "a host's reader is the SAB transport's");
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const src = `
      globalThis.__DEV__ = false;
      self.onmessage = async ({ data }) => {
        const { runSabWorker } = await import("${location.origin}/js/lib/sab_worker_loop.js");
        const { calculateOutControlIndices } = await import("${location.origin}/js/lib/ring_cursor_offsets.js");
        const { EgressReader } = await import("${location.origin}/js/lib/ring.js");
        const port = data.port;
        let reader = null;
        runSabWorker({
          name: "CountingReader", endpoint: port, initMetrics: false,
          calculateControlIndices: calculateOutControlIndices,
          headIndex: (idx) => idx.OUT_HEAD, tailIndex: (idx) => idx.OUT_TAIL,
          onInit: (ctx) => { reader = new EgressReader(ctx.rings); },
          readMessages: () => {
            const messages = [];
            reader?.poll(0, (bytes, origin, route, sequence) => messages.push({ oscData: bytes.slice(), sequence, timestamp: 0 }));
            return messages;
          },
          postResults: (messages) => port.postMessage({ type: "messages", messages }),
          extraHandlers: { loops: (_, ctx) => port.postMessage({ type: "loops", loops: ctx.liveLoops(), stale: ctx.staleReads() }) },
        });
      };`;
    const host = new Worker(URL.createObjectURL(new Blob([src], { type: "text/javascript" })), { type: "module" });
    const channel = new MessageChannel();
    host.postMessage({ port: channel.port2 }, [channel.port2]);
    const loops = () => new Promise((resolve) => {
      const on = ({ data }) => { if (data.type === "loops") { channel.port1.removeEventListener("message", on); resolve({ loops: data.loops, stale: data.stale }); } };
      channel.port1.addEventListener("message", on);
      channel.port1.postMessage({ type: "loops" });
    });
    const cw = new window.Clockwork({ ...config, oscInEndpoint: channel.port1 });
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) { host.terminate(); return { init }; }
    // Loops inside their wait once a reply has woken every one of them.
    const settled = async () => { await within(cw.sync(), 5000); return loops(); };
    const first = await settled();
    for (let i = 0; i < 3; i++) await cw.reload();
    const afterReloads = await settled();
    // Stopped and started with nothing on the ring between: a loop parked in its wait sleeps through both and
    // wakes to a running reader, beside the loop that replaced it, unless the generation it belongs to sends it away.
    for (let i = 0; i < 3; i++) { channel.port1.postMessage({ type: "stop" }); channel.port1.postMessage({ type: "start" }); }
    const later = await settled();
    await cw.shutdown();
    host.terminate();
    return { init, first, afterReloads, later };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  expect(r.first.loops, "no loop was reading").toBe(1);
  expect(r.first.stale, "a read by a loop that was stopped or replaced").toBe(0);
  expect(r.afterReloads.loops, "a loop from before a reload is still reading").toBe(1);
  expect(r.afterReloads.stale, "a loop from before a reload read after it was replaced").toBe(0);
  expect(r.later.loops, "the loop from before a stop and start is still reading").toBe(1);
  expect(r.later.stale, "the loop from before a stop and start read after it was replaced").toBe(0);
});

test("after a reload over the same memory the reply rings keep their place, and the page reads on from it", async ({ page, clockworkConfig, clockworkMode }) => {
  // A reload over the same memory onto a rate the instance cannot take replaces it, as a device change does
  // (docs/RESTARTS.md, "One event, every route"): the reply rings outlive it, so what the page had not read is still
  // there for it, and its readers read on from where they were, past the old instance's words to the new one's setup.
  // Nothing begins again at 0 under a reader, and nothing it reads is taken for damage.
  test.skip(clockworkMode !== "sab", "a reload keeps the memory only in SAB mode");
  await boot(page);
  const r = await page.evaluate(async ([config, withinSrc]) => {
    const within = eval(withinSrc);
    const offs = await import(`${location.origin}/js/lib/ring_cursor_offsets.js`);
    const cw = new window.Clockwork({ ...config });
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) return { init };
    const { transferable } = cw.createOscChannel();
    const cursors = new Int32Array(transferable.sharedBuffer,
      transferable.arenaBase + transferable.bufferConstants.RING_CURSORS_START, 16);
    const word = (byte) => byte / 4;
    const ring = (head, tail, read) => ({ head: Atomics.load(cursors, word(head)), tail: Atomics.load(cursors, word(tail)),
                                          read: Atomics.load(cursors, word(read)) });
    // The reader gives the writer its space back at the start of its next poll, and it polls again as soon as it has
    // posted a frame because the head is still ahead of the tail. So the sync's reply can reach the page before that
    // give-back has happened. Wait for the tail to catch up before sampling, with a bound: if it never catches up the
    // assertions below fail.
    const handedBack = async (head, tail, read) => {
      for (let i = 0; i < 400 && Atomics.load(cursors, word(tail)) !== Atomics.load(cursors, word(head)); i++) await new Promise((r) => setTimeout(r, 5));
      return ring(head, tail, read);
    };
    let setups = 0;
    cw.on("in:osc", ({ oscData }) => { if (window.Clockwork.osc.decode(oscData)[0] === "/clockwork/setup") setups++; });
    // The instance's ring well along before the reload: a hundred answers, read.
    for (let i = 0; i < 100; i++) cw.send("/dummy/ping");
    await within(cw.sync(), 5000);
    const before = ring(offs.OUT_HEAD, offs.OUT_TAIL, offs.OUT_READ);
    const other = cw.audioContext.sampleRate === 44100 ? 48000 : 44100;
    const reload = await within(cw.reload({ audioContext: new AudioContext({ sampleRate: other }) }), 15000);
    await within(cw.sync(), 5000);
    const after = { out: await handedBack(offs.OUT_HEAD, offs.OUT_TAIL, offs.OUT_READ),
                    nrt: await handedBack(offs.NRT_OUT_HEAD, offs.NRT_OUT_TAIL, offs.NRT_OUT_READ) };
    const corrupted = cw.getMetrics()?.oscInCorrupted;
    await cw.shutdown();
    return { init, reload, before, after, corrupted, setups };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  expect(r.reload).toEqual({ settled: true, value: true });
  expect(r.corrupted, "the reader found its own cursor damaged after the reload").toBe(0);
  expect(r.after.out.head, "the instance's ring began again under its reader").toBeGreaterThanOrEqual(r.before.head);
  expect(r.setups, "the new instance's setup, heard once").toBe(1);
  for (const [name, c] of Object.entries(r.after)) {
    expect(c.read, `the ${name} ring's reader is not where its writer is`).toBe(c.head);
    expect(c.tail, `the ${name} ring's writer has had space handed back from somewhere else`).toBe(c.head);
  }
});
