// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * Can clockwork start, at all, in a browser?
 *
 * Every one of these would have failed for most of this project's life, and
 * none of them could have been written: there was no page, no server, no
 * config, and no JS build, so nothing clockwork owned could construct its
 * own client. The defects that shipped as a result were not subtle —
 * cell_pool.js was referenced and never written, midi_manager.js had one
 * import statement pasted inside another and never parsed — they were simply
 * unobservable from here.
 *
 * The guest is dsp/dummy. It declares holds_schedule 0, so these also
 * exercise clockwork's scheduler, which accepted nothing at all in wasm
 * until 2026-08-31.
 */
import { test, expect, boot } from "./fixtures.mjs";

test("the client module loads without a page error", async ({ page }) => {
  const errors = await boot(page);
  expect(errors, "loading the client logged errors").toEqual([]);
  expect(await page.evaluate(() => typeof window.Clockwork)).toBe("function");
});

test("clockwork boots and reports its guest", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();
    const out = { initialized: clockwork.initialized, mode: clockwork.mode, dsp: clockwork.dspName ?? null };
    await clockwork.shutdown();
    return out;
  }, clockworkConfig);

  expect(r.initialized, "init() resolved but the client says it is not up").toBe(true);
});

test("what the engine says as it boots reaches the 'debug' event", async ({ page, clockworkConfig }) => {
  // The banner, the version, "engine ready": written to the egress before
  // anything on the main thread is listening. On the SAB transport the frames
  // wait in the ring for the worker that drains it; on postMessage the worklet
  // drained them itself and posted them to a port with no handler yet.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    const texts = [];
    const sequences = [];
    clockwork.on('debug', (m) => texts.push(m.text));
    clockwork.on('in:osc', (m) => sequences.push(m.sequence));
    await clockwork.init();
    await new Promise((r) => setTimeout(r, 500));
    await clockwork.shutdown();
    return { texts, firstSequence: sequences[0] ?? null };
  }, clockworkConfig);

  expect(r.texts.join("\n")).toContain("> engine ready...");
  // Frame 0 is the first thing the engine ever said. Anything else here means
  // frames left the egress before anyone could hear them.
  expect(r.firstSequence, "the first frame seen was not the first frame sent").toBe(0);
});

test("a message reaches the guest and the reply comes back", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();
    // The whole round trip in one assertion: client -> ingress ring ->
    // worklet -> dsp_osc -> emit_osc -> egress -> client.
    const pong = new Promise((resolve) => {
      const t = setTimeout(() => resolve(null), 4000);
      clockwork.on("in:osc", ({ oscData }) => {
        const s = new TextDecoder().decode(oscData.subarray(0, 16));
        if (s.startsWith("/dummy/pong")) { clearTimeout(t); resolve(true); }
      });
    });
    clockwork.send("/dummy/ping");
    const got = await pong;
    await clockwork.shutdown();
    return { got };
  }, clockworkConfig);

  expect(r.got, "no /dummy/pong came back — the round trip is broken").toBe(true);
});

test("the inbox grows, and what was already in it survives", async ({ page, clockworkConfig }) => {
  // BULK IS WHAT GROWS. The inbox is the client's way to hand the guest a
  // sample, a wavetable, an impulse response — and a client loading samples
  // does not know at boot how many it will load. The guest's arena does not
  // grow for anybody (nothing calls for it) while this does, so the growable
  // tail of the memory belongs here.
  //
  // Growth must not disturb what is already published: a guest reading a range
  // in place keeps it until it says otherwise, so an earlier write has to
  // still be there afterwards, at the same offset.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();

    const before = clockwork.inbox();
    const mark = new Uint8Array([0xC1, 0x0C, 0x6B, 0x21]);

    // Right at the old end, so growth cannot quietly relocate the region
    // without this moving too.
    await clockwork.writeInbox(before.size - mark.length, mark);

    const grew = await clockwork.growInbox(1024 * 1024);
    const after = clockwork.inbox();

    // A write PAST the old end, which the range check refused a moment ago.
    let wrotePastOldEnd = false;
    try {
      await clockwork.writeInbox(before.size, mark);
      wrotePastOldEnd = true;
    } catch { /* reported below */ }

    const read = await clockwork.readInboxForTest?.(before.size - mark.length, mark.length);
    await clockwork.shutdown();
    return {
      grew, wrotePastOldEnd,
      beforeOffset: before.offset, afterOffset: after.offset,
      beforeSize: before.size, afterSize: after.size,
      maxSize: before.maxSize ?? null,
      read: read ? Array.from(read) : null,
    };
  }, clockworkConfig);

  expect(r.grew, "growInbox refused").toBe(true);
  expect(r.afterSize, "the inbox did not get bigger").toBeGreaterThan(r.beforeSize);
  expect(r.afterOffset, "the inbox moved, so every published range is now wrong")
    .toBe(r.beforeOffset);
  expect(r.wrotePastOldEnd, "a write past the old end was still refused").toBe(true);
});

test("a /clockwork/ system verb answers the client that asked, on either transport", async ({ page, clockworkConfig }) => {
  // The guest is not the only thing that replies. Verbs under /clockwork/
  // are answered by clockwork itself, some of them ON THE AUDIO THREAD
  // (clockwork_sys.h: clock/state/get answers there because a caller that
  // wants the clock the current block is rendered against cannot wait for an
  // NRT hop). That reply travels the same egress a guest's does, and a client
  // that can hear /dummy/pong but not this has a hole in its transport that
  // only a system verb falls through.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();
    const reply = new Promise((resolve) => {
      const t = setTimeout(() => resolve(null), 4000);
      clockwork.on("in:osc", ({ oscData }) => {
        const s = new TextDecoder().decode(oscData.subarray(0, 32));
        if (s.startsWith("/clockwork/clock/state.reply")) { clearTimeout(t); resolve(true); }
      });
    });
    clockwork.send("/clockwork/clock/state/get");
    const got = await reply;
    await clockwork.shutdown();
    return { got };
  }, clockworkConfig);
  expect(r.got, "no /clockwork/clock/state.reply came back").toBe(true);
});

test("what was sent is watched, on either transport", async ({ page, clockworkConfig }) => {
  // The 'out:osc' event is fed by a TAP on the ingress ring: a reader with a
  // cursor of its own that takes nothing, because the engine is that ring's
  // consumer and a second drain would be eating its input.
  //
  // Both transports reach it by a different route — postMessage mode taps from
  // inside the worklet, SAB mode from a worker with its own module instance —
  // so this is the assertion that the tap works in a browser at all, and that
  // watching has not quietly started consuming.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();

    const watched = new Promise((resolve) => {
      const t = setTimeout(() => resolve(null), 4000);
      clockwork.on("out:osc", ({ oscData }) => {
        const s = new TextDecoder().decode(oscData.subarray(0, 16));
        if (s.startsWith("/dummy/ping")) { clearTimeout(t); resolve(true); }
      });
    });

    // The same message the round trip uses, so a reply still has to arrive:
    // watching it must not have taken it away from the engine.
    const pong = new Promise((resolve) => {
      const t = setTimeout(() => resolve(null), 4000);
      clockwork.on("in:osc", ({ oscData }) => {
        const s = new TextDecoder().decode(oscData.subarray(0, 16));
        if (s.startsWith("/dummy/pong")) { clearTimeout(t); resolve(true); }
      });
    });

    clockwork.send("/dummy/ping");
    const out = { saw: await watched, replied: await pong };
    await clockwork.shutdown();
    return out;
  }, clockworkConfig);

  expect(r.saw, "the send was never seen — the ingress tap is not delivering").toBe(true);
  expect(r.replied, "the tap consumed the message instead of watching it").toBe(true);
});

test("the guest's window reaches the client whole, on either transport", async ({ page, clockworkConfig }) => {
  // readWindow() hands back bytes and where in them the window starts. The two
  // transports get there differently: SAB mode points into the shared memory,
  // while postMessage mode reads a SNAPSHOT the worklet copies out — metrics
  // followed by the window, contiguous because shared_memory.h puts them that
  // way for this copy.
  //
  // So the snapshot has to be measured with the same constant at both ends. It
  // was not: the worklet sized it with a name that no longer existed, which is
  // undefined in JavaScript rather than an error, so the length came out NaN
  // and the copy came out EMPTY. Nothing failed at the seam — readWindow()
  // returned an offset and a size quite happily, and the first consumer to
  // cast the buffer threw instead.
  //
  // Casting it is therefore the assertion. A consumer reads the version stamp
  // at the front, which is the one field the window's contract fixes.
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();
    const w = clockwork.readWindow();
    const out = w && {
      offset: w.offset,
      size: w.size,
      byteLength: w.buffer.byteLength,
      // Where a short buffer actually bites.
      version: new Uint32Array(w.buffer, w.offset, 1)[0],
    };
    await clockwork.shutdown();
    return out;
  }, clockworkConfig);

  expect(r, "readWindow() gave nothing back").not.toBeNull();
  expect(r.size, "the window has no size").toBeGreaterThan(0);
  expect(r.byteLength,
    `the buffer ends before the window does: ${r.byteLength} bytes for a window ` +
    `of ${r.size} at offset ${r.offset}`).toBeGreaterThanOrEqual(r.offset + r.size);
  expect(Number.isInteger(r.version), "the version stamp did not read back").toBe(true);
});

test("the bulk lanes are one-way, and both carry, on either transport", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();

    // Two regions, one writer each. The client writes the inbox and the guest
    // only reads it; the guest writes the outbox and the client only reads it.
    // That is the whole reason there are two: with a single region both ends
    // wrote into the same bytes and nothing arbitrated between them.
    const inbox  = clockwork.inbox();
    const outbox = clockwork.outbox();
    const arena  = clockwork.guestMemory();

    const overlaps = (a, b) =>
      a.offset < b.offset + b.size && b.offset < a.offset + a.size;

    // The bytes both sides compute independently, so an assertion on them is
    // an assertion about transport rather than about agreement. Same two
    // functions as dsp/dummy/dummy_dsp.cpp.
    const byteAt = (seed, i) => (seed * 31 + i * 7 + (i >>> 8)) & 0xff;
    const fnv1a = (bytes) => {
      let h = 2166136261;
      for (const b of bytes) h = Math.imul(h ^ b, 16777619) >>> 0;
      return h >>> 0;
    };

    // Replies come back as OSC on the egress lane, same path as /dummy/pong.
    // decode() hands back a flat [address, ...args], not an object.
    //
    // The arguments are read back UNSIGNED. A hash is a uint32 on the guest's
    // side and an OSC int32 on the wire, so anything with the top bit set
    // arrives here negative and would never equal the number the client
    // computed.
    const reply = (address) => new Promise((resolve) => {
      const t = setTimeout(() => resolve(null), 4000);
      clockwork.on("in:osc", ({ oscData }) => {
        const [addr, ...args] = window.Clockwork.osc.decode(oscData);
        if (addr === address) {
          clearTimeout(t);
          resolve(args.map((a) => a >>> 0));
        }
      });
    });

    // ── Client to guest ─────────────────────────────────────────────────────
    // OFFSETS ARE REGION-RELATIVE. Native maps the same segment at its own
    // address in each process, so an absolute pointer is the one thing that
    // cannot travel; both sides count from the region base.
    const staged = new Uint8Array(4096);
    for (let i = 0; i < staged.length; i++) staged[i] = byteAt(7, i);
    await clockwork.writeInbox(1024, staged);

    const inboxIs = reply("/dummy/inbox/is");
    clockwork.send("/dummy/inbox/hash", 1024, staged.length);
    const sawInbox = await inboxIs;

    // ── Guest to client ─────────────────────────────────────────────────────
    const wants = 4096, seed = 99;
    const blobAt = reply("/dummy/blob/at");
    clockwork.send("/dummy/blob/produce", wants, seed);
    const produced = await blobAt;

    let readBack = null, readHash = null;
    if (produced) {
      const [offset, wrote] = produced;
      readBack = Array.from(await clockwork.readOutbox(offset, wrote));
      readHash = fnv1a(readBack);
    }

    const expected = [];
    for (let i = 0; i < wants; i++) expected.push(byteAt(seed, i));

    // Out of range is refused rather than clamped, on both transports, and
    // before anything is written.
    let refused = null;
    try { await clockwork.writeInbox(inbox.size - 2, new Uint8Array(8)); }
    catch (e) { refused = String(e); }

    await clockwork.shutdown();
    return {
      inbox, outbox,
      inboxHitsArena:  overlaps(inbox, arena),
      outboxHitsArena: overlaps(outbox, arena),
      lanesOverlap:    overlaps(inbox, outbox),
      sawInbox, stagedHash: fnv1a(staged), stagedLen: staged.length,
      produced, readHash, matches: readBack !== null
        && readBack.length === expected.length
        && readBack.every((b, i) => b === expected[i]),
      refused,
    };
  }, clockworkConfig);

  expect(r.inbox.size, "no inbox reported").toBeGreaterThan(0);
  expect(r.outbox.size, "no outbox reported").toBeGreaterThan(0);

  // Disjointness is the invariant, not a layout detail: an inbox that overlaps
  // the arena is a client with a pointer into the guest's allocator, which is
  // exactly the arrangement these two regions replaced.
  expect(r.lanesOverlap, "the inbox and the outbox share bytes").toBe(false);
  expect(r.inboxHitsArena, "the inbox overlaps the guest's arena").toBe(false);
  expect(r.outboxHitsArena, "the outbox overlaps the guest's arena").toBe(false);

  // The guest saw what the client staged, at the offset it was told.
  expect(r.sawInbox, "the guest never answered /dummy/inbox/hash").not.toBe(null);
  expect(r.sawInbox[0], "the guest read a different number of bytes").toBe(r.stagedLen);
  expect(r.sawInbox[1], "the guest hashed different bytes than were staged").toBe(r.stagedHash);

  // BOTH TRANSPORTS. Reading back used to throw in postMessage mode, on the
  // true observation that the client cannot see the worklet's heap — but the
  // worklet can, so it does the copy and transfers the result.
  expect(r.produced, "the guest never answered /dummy/blob/produce").not.toBe(null);
  expect(r.produced[1], "the guest wrote nothing into the outbox").toBeGreaterThan(0);
  expect(r.readHash, "the bytes read back hash differently than the guest's").toBe(r.produced[2]);
  expect(r.matches, "the bytes read back are not the blob the guest generated").toBe(true);

  expect(r.refused, "a range past the end of the inbox was accepted").toContain("outside the region");
});

test("with no mode given, the transport is SAB where the page is isolated and postMessage elsewhere", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const { mode: _given, ...rest } = config;
    const clockwork = new window.Clockwork(rest);
    await clockwork.init();
    const out = { mode: clockwork.mode, isolated: window.crossOriginIsolated };
    await clockwork.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.mode).toBe(r.isolated ? "sab" : "postMessage");
});

test("request() is one message and its reply; a refusal or a silence rejects", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();
    const pong = await clockwork.request("/dummy/ping", [], { reply: "/dummy/pong" });
    let refused = null, silent = null;
    // The pong arriving on the address named as the error is a refusal.
    try { await clockwork.request("/dummy/ping", [], { reply: "/never", error: "/dummy/pong" }); }
    catch (e) { refused = e.message; }
    try { await clockwork.request("/dummy/ping", [], { reply: "/never", timeoutMs: 300 }); }
    catch (e) { silent = e.message; }
    let noReply = null;
    try { clockwork.request("/dummy/ping"); } catch (e) { noReply = e.message; }
    await clockwork.shutdown();
    return { pong: pong[0], refused, silent, noReply };
  }, clockworkConfig);
  expect(r.pong).toBe("/dummy/pong");
  expect(r.refused).toMatch(/refused/);
  expect(r.silent).toMatch(/no \/never to \/dummy\/ping within 300 ms/);
  expect(r.noReply).toMatch(/needs the reply address/);
});

test("sync() is clockwork's own barrier: it answers for the placeholder guest, after what was sent before", async ({ page, clockworkConfig }) => {
  await boot(page);
  const r = await page.evaluate(async (config) => {
    const clockwork = new window.Clockwork(config);
    await clockwork.init();
    const seen = [];
    clockwork.on("in", (m) => seen.push(m[0]));
    clockwork.send("/dummy/ping");
    await clockwork.sync(4242);
    const out = { pongBeforeSynced: seen.indexOf("/dummy/pong") >= 0 && seen.indexOf("/dummy/pong") < seen.indexOf("/clockwork/synced"), seen };
    await clockwork.shutdown();
    return out;
  }, clockworkConfig);
  expect(r.pongBeforeSynced, `the pong sent before the barrier came back before it: ${r.seen}`).toBe(true);
});
