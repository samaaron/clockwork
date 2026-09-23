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
    // more reloads than there are client slots: a slot leaked, or given back twice, by any reload shows up here
    const reloads = (cw.bufferConstants?.CLIENT_SLOT_COUNT ?? 4) + 2;
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
  expect(r.reloadFailed ?? null, "a reload failed: client slots ran out, or two clients were given one").toBeNull();
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
        const { calculateOutControlIndices } = await import("${location.origin}/js/lib/control_offsets.js");
        const port = data.port;
        let passes = 0;
        runSabWorker({
          name: "CountingReader", endpoint: port, initMetrics: false,
          calculateControlIndices: calculateOutControlIndices,
          headIndex: (idx) => idx.OUT_HEAD, tailIndex: (idx) => idx.OUT_TAIL,
          readMessages: (ctx) => {
            passes++;
            const messages = [];
            ctx.client?.poll((bytes, origin, route, sequence) => messages.push({ oscData: bytes.slice(), sequence, timestamp: 0 }));
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
