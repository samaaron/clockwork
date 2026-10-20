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
  // Counted from inside: the pump's own read, with every pass of it counted. One loop makes a pass each time the ring
  // moves; a loop left over from each reload wakes with it too, and makes one more.
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
        let passes = 0;
        let reader = null;
        runSabWorker({
          name: "CountingReader", endpoint: port, initMetrics: false,
          calculateControlIndices: calculateOutControlIndices,
          headIndex: (idx) => idx.OUT_HEAD, tailIndex: (idx) => idx.OUT_TAIL,
          onInit: (ctx) => { reader = new EgressReader(ctx.rings); },
          readMessages: () => {
            passes++;
            const messages = [];
            reader?.poll(0, (bytes, origin, route, sequence) => messages.push({ oscData: bytes.slice(), sequence, timestamp: 0 }));
            return messages;
          },
          postResults: (messages) => port.postMessage({ type: "messages", messages }),
          extraHandlers: { passes: () => port.postMessage({ type: "passes", passes }) },
        });
      };`;
    const host = new Worker(URL.createObjectURL(new Blob([src], { type: "text/javascript" })), { type: "module" });
    const channel = new MessageChannel();
    host.postMessage({ port: channel.port2 }, [channel.port2]);
    const passes = () => new Promise((resolve) => {
      const on = ({ data }) => { if (data.type === "passes") { channel.port1.removeEventListener("message", on); resolve(data.passes); } };
      channel.port1.addEventListener("message", on);
      channel.port1.postMessage({ type: "passes" });
    });
    const cw = new window.Clockwork({ ...config, oscInEndpoint: channel.port1 });
    const init = await within(cw.init(), 10000);
    if (init.error || !init.settled) { host.terminate(); return { init }; }
    // what one reply costs in passes, before any reload and after three
    const cost = async () => {
      await new Promise((r) => setTimeout(r, 200));
      const before = await passes();
      await within(cw.sync(), 5000);
      await new Promise((r) => setTimeout(r, 200));
      return (await passes()) - before;
    };
    const first = await cost();
    for (let i = 0; i < 3; i++) await cw.reload();
    const afterReloads = await cost();
    // A reload's own traffic usually wakes the old loop while the reader is stopped, and it ends. With nothing on the
    // ring between 'stop' and 'start' it sleeps through both — and wakes, to a running reader, beside the new loop.
    for (let i = 0; i < 3; i++) { channel.port1.postMessage({ type: "stop" }); channel.port1.postMessage({ type: "start" }); }
    const later = await cost();
    await cw.shutdown();
    host.terminate();
    return { init, first, afterReloads, later };
  }, [clockworkConfig, within]);
  expect(r.init.error ?? null).toBeNull();
  expect(r.first, "the reader made no pass when a reply arrived").toBeGreaterThan(0);
  expect(r.afterReloads, "more passes per reply after reloads: a loop from before a reload is still reading").toBe(r.first);
  expect(r.later, "more passes per reply after a stop and start: the loop from before is still reading").toBe(r.first);
});

test("after a reload over the same memory the reply rings keep their place, and the page reads on from it", async ({ page, clockworkConfig, clockworkMode }) => {
  // A reload over the same memory replaces the instance as a device change does (docs/RESTARTS.md, "One event,
  // every route"): the reply rings outlive it, so what the page had not read is still there for it, and its readers
  // read on from where they were, past the old instance's words to the new one's setup. Nothing begins again at 0
  // under a reader, and nothing it reads is taken for damage.
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
    let setups = 0;
    cw.on("in:osc", ({ oscData }) => { if (window.Clockwork.osc.decode(oscData)[0] === "/clockwork/setup") setups++; });
    // The instance's ring well along before the reload: a hundred answers, read.
    for (let i = 0; i < 100; i++) cw.send("/dummy/ping");
    await within(cw.sync(), 5000);
    const before = ring(offs.OUT_HEAD, offs.OUT_TAIL, offs.OUT_READ);
    const reload = await within(cw.reload(), 15000);
    await within(cw.sync(), 5000);
    const after = { out: ring(offs.OUT_HEAD, offs.OUT_TAIL, offs.OUT_READ),
                    nrt: ring(offs.NRT_OUT_HEAD, offs.NRT_OUT_TAIL, offs.NRT_OUT_READ) };
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
