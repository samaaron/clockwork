// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// js/lib/ring.js against the compiled C, on the engine's own rings.
//
// The corpus (ring_wire.test.mjs) pins the format on a handful of fixed
// cases, from a zeroed ring. This runs thousands of writes and reads of random
// sizes through a booted engine, so the rings wrap many times over bytes
// earlier frames left behind, and fill, and refuse:
//
//   - C writes ingress (clockwork_client_send) while a JavaScript writer
//     writes the same frames to a copy of the ring. Every accept and refusal
//     must agree, JavaScript reads the engine's ring, and at the end the two
//     rings must be the same bytes.
//   - JavaScript writes both egress rings while clockwork_client_poll reads
//     them, beside copies read by EgressReader. Every delivery, cursor and
//     count must agree.
//   - a C tap and a JavaScript tap watch ingress while it is written past
//     them. They must see the same frames, and miss the same number.
//
// The seed is fixed, so a failure happens again the same way.
import { test } from "node:test";
import assert from "node:assert/strict";

import { ringAt, ringWrite, ringDrain, RingTap, EgressReader } from "../js/lib/ring.js";
import * as M from "../js/lib/metrics_offsets.js";
import { bootEngine, haveWasm, testIf } from "./engine_boot.mjs";

const maybe = testIf(haveWasm, "dist/wasm/clockwork.wasm");

const SEED = 0xC10C;
const REGION_INGRESS = 7;     // ClockworkRegionId
const MESSAGE_BYTES = 20;     // ClockworkClientMessage: bytes, length, origin, route, sequence
const ROUTE_BYTES = 4;        // an egress frame is [route:u32][osc]

// mulberry32: small, and the same sequence on every machine.
function random(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const below = (rand, n) => Math.floor(rand() * n);
function bytesOf(rand, n) {
  const b = new Uint8Array(n);
  for (let i = 0; i < n; i++) b[i] = below(rand, 256);
  return b;
}

// A copy of a ring in memory of its own: the same bytes and cursor values,
// and its own words, so JavaScript can run beside the C from the same start.
function copyOf(ring) {
  const size = ring.bytes.length;
  const words = new Int32Array(5);
  const copy = ringAt(new ArrayBuffer(size), 0, size, words, {
    head: 0, tail: 1, sequence: 2,
    lock: ring.lock === undefined ? undefined : 3,
    read: ring.read === undefined ? undefined : 4,
  });
  copy.bytes.set(ring.bytes);
  words[0] = Atomics.load(ring.words, ring.head);
  words[1] = Atomics.load(ring.words, ring.tail);
  words[2] = Atomics.load(ring.words, ring.sequence);
  if (ring.read !== undefined) words[4] = Atomics.load(ring.words, ring.read);
  return copy;
}

// What a ClockworkClientMessage array holds after a poll, as plain values.
function messagesAt(memory, at, count) {
  const view = new DataView(memory.buffer);
  const out = [];
  for (let i = 0; i < count; i++) {
    const p = at + i * MESSAGE_BYTES;
    const ptr = view.getUint32(p, true), length = view.getUint32(p + 4, true);
    out.push({
      origin: view.getUint32(p + 8, true),
      route: view.getUint32(p + 12, true),
      sequence: view.getUint32(p + 16, true),
      bytes: new Uint8Array(memory.buffer, ptr, length).slice(),
    });
  }
  return out;
}

const cursors = (ring) => ({
  head: Atomics.load(ring.words, ring.head),
  tail: Atomics.load(ring.words, ring.tail),
  sequence: Atomics.load(ring.words, ring.sequence),
  ...(ring.read === undefined ? {} : { read: Atomics.load(ring.words, ring.read) }),
});

maybe("C writes ingress and JavaScript reads it, and a JavaScript writer leaves the same bytes", async () => {
  const { x, memory, handle, rings } = await bootEngine();
  const ring = rings.ingress;
  const copy = copyOf(ring);
  const size = ring.bytes.length;
  const rand = random(SEED);
  const scratch = x.malloc(size);

  const fromRing = { words: ring.words, index: ring.tail };
  const fromCopy = { words: copy.words, index: copy.tail };
  const stRing = { lastSeq: -1 }, stCopy = { lastSeq: -1 };
  const expected = [];
  let accepted = 0, refused = 0, wraps = 0, read = 0;

  for (let op = 0; op < 4000; op++) {
    if (rand() < 0.8) {
      // Mostly small, now and then a third of the ring; read more slowly than
      // written, so it fills and refuses.
      const length = rand() < 0.9 ? 1 + below(rand, 4096) : 1 + below(rand, Math.floor(size / 3));
      const payload = bytesOf(rand, length);
      const origin = below(rand, 2 ** 32);
      const headBefore = Atomics.load(ring.words, ring.head);

      new Uint8Array(memory.buffer, scratch, length).set(payload);
      const byC = x.clockwork_client_send(handle, scratch, length, origin) === 0;
      const byJs = ringWrite(copy, payload, origin);
      assert.equal(byJs, byC, `op ${op}: a ${length}-byte write, accepted by C ${byC}, by JavaScript ${byJs}`);

      if (byC) {
        accepted++;
        expected.push({ origin, bytes: payload });
        if (Atomics.load(ring.words, ring.head) < headBefore) wraps++;
      } else {
        refused++;
      }
    } else {
      const max = rand() < 0.05 ? 0 : 1 + below(rand, 3);   // 0 is everything
      const fromC = [], fromJs = [];
      ringDrain(ring, fromRing, stRing, max, (origin, offset, length, sequence) =>
        fromC.push({ origin, sequence, bytes: ring.bytes.slice(offset, offset + length) }));
      ringDrain(copy, fromCopy, stCopy, max, (origin, offset, length, sequence) =>
        fromJs.push({ origin, sequence, bytes: copy.bytes.slice(offset, offset + length) }));
      assert.deepEqual(fromJs, fromC, `op ${op}: the two rings deliver the same frames`);
      for (const m of fromC) {
        const want = expected.shift();
        assert.equal(m.origin, want.origin >>> 0, `op ${op}: origin`);
        assert.deepEqual(m.bytes, want.bytes, `op ${op}: the bytes sent are the bytes read`);
        read++;
      }
    }
  }

  assert.deepEqual(cursors(copy), { ...cursors(ring) }, "the same cursors");
  assert.ok(Buffer.compare(Buffer.from(copy.bytes), Buffer.from(ring.bytes)) === 0,
    "the C-written ring and the JavaScript-written ring are the same bytes");
  assert.ok(accepted > 1000 && refused > 50 && wraps > 5 && read > 1000,
    `the run covered what it is for: ${accepted} written, ${refused} refused, ${wraps} wraps, ${read} read`);
});

maybe("JavaScript writes both egress rings, and EgressReader reads them as clockwork_client_poll does", async () => {
  const { x, memory, handle, rings, arenaBase, constants } = await bootEngine();
  const max = 32;
  const messages = x.malloc(max * MESSAGE_BYTES);

  // Whatever the engine said while it booted is taken first, so the copies
  // start from rings with nothing on them but this test's frames.
  while (x.clockwork_client_poll(handle, messages, max) > 0) { /* drained */ }
  x.clockwork_client_poll(handle, messages, max);   // and its space given back

  // The engine's counters, which clockwork_client_poll adds to, and a set of
  // the reader's own, which must move by the same amounts.
  const engineMetrics = new Uint32Array(memory.buffer, arenaBase + constants.METRICS_START,
                                        constants.METRICS_SIZE / 4);
  const counted = [M.OSC_IN_MESSAGES_RECEIVED, M.OSC_IN_BYTES_RECEIVED, M.OSC_IN_CORRUPTED, M.ENGINE_SEQUENCE_GAPS];
  const before = counted.map((i) => engineMetrics[i]);
  const ownMetrics = new Uint32Array(constants.METRICS_SIZE / 4);
  const engineMoved = () => counted.map((i, k) => engineMetrics[i] - before[k]);
  const ownMoved = () => counted.map((i) => ownMetrics[i]);

  const copies = { egress: copyOf(rings.egress), nrtEgress: copyOf(rings.nrtEgress) };
  const reader = new EgressReader(copies, {
    view: ownMetrics, received: M.OSC_IN_MESSAGES_RECEIVED, bytes: M.OSC_IN_BYTES_RECEIVED,
    corrupted: M.OSC_IN_CORRUPTED, seqGaps: M.ENGINE_SEQUENCE_GAPS,
  });
  const rand = random(SEED + 1);
  const expected = { egress: [], nrtEgress: [] };
  let accepted = 0, refused = 0, wraps = 0, empty = 0, read = 0, fromNrt = 0;

  for (let op = 0; op < 6000; op++) {
    if (rand() < 0.8) {
      // A frame is [route][osc], on either ring. One in twenty carries
      // nothing at all, which both readers consume without delivering.
      const which = rand() < 0.5 ? "egress" : "nrtEgress";
      const ring = rings[which];
      const size = ring.bytes.length;
      const none = rand() < 0.05;
      const oscBytes = rand() < 0.9 ? below(rand, 2048) : below(rand, Math.floor(size / 3));
      const payload = none ? new Uint8Array(0) : bytesOf(rand, ROUTE_BYTES + oscBytes);
      const origin = below(rand, 2 ** 32);
      const headBefore = Atomics.load(ring.words, ring.head);

      const onEngine = ringWrite(ring, payload, origin);
      const onCopy = ringWrite(copies[which], payload, origin);
      assert.equal(onCopy, onEngine, `op ${op}: both ${which} rings take or refuse the frame alike`);
      if (onEngine) {
        accepted++;
        if (none) empty++;
        else expected[which].push({ origin, bytes: payload });
        if (Atomics.load(ring.words, ring.head) < headBefore) wraps++;
      } else {
        refused++;
      }
    } else {
      const want = 1 + below(rand, rand() < 0.05 ? max : 3);
      const byC = messagesAt(memory, messages, x.clockwork_client_poll(handle, messages, want));
      const byJs = [];
      reader.poll(want, (bytes, origin, route, sequence) =>
        byJs.push({ origin, route, sequence, bytes: bytes.slice() }));

      assert.deepEqual(byJs, byC, `op ${op}: C and JavaScript read the same frames`);
      for (const which of ["egress", "nrtEgress"])
        assert.deepEqual(cursors(copies[which]), cursors(rings[which]), `op ${op}: the ${which} cursors agree`);
      assert.deepEqual(ownMoved(), engineMoved(), `op ${op}: and they counted the same`);

      // RT first, then NRT, each in the order written.
      for (const m of byC) {
        const head = expected.egress[0];
        const which = head && head.origin >>> 0 === m.origin
          && m.bytes.length === head.bytes.length - ROUTE_BYTES ? "egress" : "nrtEgress";
        const sent = expected[which].shift();
        assert.equal(m.origin, sent.origin >>> 0, `op ${op}: origin`);
        assert.equal(m.route, new DataView(sent.bytes.buffer).getUint32(0, true), `op ${op}: route`);
        assert.deepEqual(m.bytes, sent.bytes.subarray(ROUTE_BYTES), `op ${op}: the bytes written are the bytes read`);
        if (which === "nrtEgress") fromNrt++;
        read++;
      }
    }
  }

  assert.ok(accepted > 1000 && refused > 50 && wraps > 5 && empty > 20 && read > 1000 && fromNrt > 300,
    `the run covered what it is for: ${accepted} written (${empty} empty), ${refused} refused, ${wraps} wraps, `
    + `${read} read (${fromNrt} from NRT)`);
});

maybe("a C tap and a JavaScript tap watching ingress see the same frames and miss the same number", async () => {
  const { x, memory, handle, status, rings } = await bootEngine();
  const ring = rings.ingress;
  const size = ring.bytes.length;
  const rand = random(SEED + 2);
  const scratch = x.malloc(size);
  const max = 32;
  const messages = x.malloc(max * MESSAGE_BYTES);

  // Both open at the same moment, so both start at the same head.
  const tapBytes = x.clockwork_client_tap_sizeof();
  const cTap = x.clockwork_client_tap_open_in(x.malloc(tapBytes), tapBytes, handle, REGION_INGRESS, status);
  assert.ok(cTap, "the C tap opened");
  const jsTap = new RingTap(ring);

  // The engine is not running, so this stands in for it, taking from ingress
  // to make room. The taps watch; they are written past when they lag.
  const fromRing = { words: ring.words, index: ring.tail };
  const st = { lastSeq: -1 };
  let polls = 0, seen = 0;

  for (let op = 0; op < 6000; op++) {
    const r = rand();
    if (r < 0.6) {
      const length = rand() < 0.9 ? 1 + below(rand, 4096) : 1 + below(rand, Math.floor(size / 3));
      new Uint8Array(memory.buffer, scratch, length).set(bytesOf(rand, length));
      x.clockwork_client_send(handle, scratch, length, below(rand, 2 ** 32));
    } else if (r < 0.9) {
      ringDrain(ring, fromRing, st, below(rand, 16), () => {});
    } else {
      // Seldom enough that they fall behind and are lapped.
      const want = 1 + below(rand, max);
      const byC = messagesAt(memory, messages, x.clockwork_client_tap_poll(cTap, messages, want))
        .map(({ origin, sequence, bytes }) => ({ origin, sequence, bytes }));
      const byJs = [];
      jsTap.poll(want, (origin, offset, length, sequence) =>
        byJs.push({ origin, sequence, bytes: ring.bytes.slice(offset, offset + length) }));
      assert.deepEqual(byJs, byC, `op ${op}: the two taps see the same frames`);
      assert.equal(jsTap.missed, Number(x.clockwork_client_tap_missed(cTap)),
        `op ${op}: and have missed the same number`);
      polls++;
      seen += byC.length;
    }
  }

  assert.ok(polls > 300 && seen > 1000 && jsTap.missed > 100,
    `the run covered what it is for: ${polls} polls, ${seen} frames seen, ${jsTap.missed} missed`);
});

maybe("at the ring's edges, C and JavaScript take and refuse the same writes and leave the same bytes", async () => {
  // The random runs rarely land a frame exactly on a boundary, and a mistake
  // of one byte there is the kind that tears a message once in a long while.
  // So every writer and reader position near the end and the start is tried,
  // with every frame size up to 96 bytes, over bytes left behind by earlier
  // frames (a byte the writer should have zeroed and did not shows here).
  const { x, memory, handle, rings } = await bootEngine();
  const ring = rings.ingress;
  const copy = copyOf(ring);
  const size = ring.bytes.length;
  const scratch = x.malloc(128);
  const STALE = 0xA5;

  const steps = (from, to) => { const out = []; for (let v = from; v <= to; v += 4) out.push(v); return out; };
  const places = [
    [0, 0],
    // the writer near the end, the reader near the start: wraps, and room
    // that is enough in total but not in one piece
    ...steps(size - 48, size - 4).flatMap((h) => steps(0, 48).map((t) => [h, t])),
    // the writer just behind the reader, after a wrap
    ...steps(0, 48).flatMap((h) => steps(h + 4, h + 96).map((t) => [h, t])),
  ];
  const touched = (r) => Buffer.concat([
    Buffer.from(r.bytes.subarray(size - 128)), Buffer.from(r.bytes.subarray(0, 256))]);

  let took = 0, refused = 0, wrapped = 0;
  for (const [h, t] of places) {
    for (let length = 1; length <= 96; length++) {
      for (const r of [ring, copy]) {
        r.bytes.fill(STALE, size - 128);
        r.bytes.fill(STALE, 0, 256);
        Atomics.store(r.words, r.head, h);
        Atomics.store(r.words, r.tail, t);
      }
      const payload = new Uint8Array(length).map((_, i) => (i * 7 + length) & 0xFF);
      new Uint8Array(memory.buffer, scratch, length).set(payload);

      const byC = x.clockwork_client_send(handle, scratch, length, 7) === 0;
      const byJs = ringWrite(copy, payload, 7);
      const where = `head ${h}, tail ${t}, ${length} bytes`;
      assert.equal(byJs, byC, `${where}: taken by C ${byC}, by JavaScript ${byJs}`);
      assert.deepEqual(cursors(copy), cursors(ring), `${where}: the same cursors`);
      assert.ok(touched(copy).equals(touched(ring)), `${where}: the same bytes`);

      if (!byC) refused++;
      else if (Atomics.load(ring.words, ring.head) < h) wrapped++;
      else took++;
    }
  }
  assert.ok(took > 1000 && refused > 1000 && wrapped > 1000,
    `every kind of write happened: ${took} in place, ${wrapped} wrapped, ${refused} refused`);
});
