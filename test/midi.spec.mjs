// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * MIDI and gamepad in a browser, end to end.
 *
 * The worklet has no thread to answer /clockwork/midi/ or /clockwork/gamepad/
 * on and no device to answer them with: Web MIDI and the Gamepad API exist on
 * the main thread only. So the audio thread forwards every such verb back to
 * the client that sent it, wrapped in a bundle carrying the call's time, and
 * the client's front (js/lib/host_front.js) answers it there through the
 * same Rust cores the native engine links. This is the whole loop, in a real
 * browser, on both transports: client -> ingress -> worklet -> egress ->
 * front -> device, and device -> front -> client.
 *
 * The devices are fakes (web_fakes.mjs, injected before the page loads and
 * installed over navigator.requestMIDIAccess / navigator.getGamepads), so the
 * bytes a port was sent and the timestamp they were sent with can be read
 * back. Everything between the client and the fake is real.
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { test, expect, boot } from "./fixtures.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
// The fakes as a classic script: the same file the node suites import, with
// its `export` keywords dropped, so both kinds of test see one fake.
const fakes = fs.readFileSync(path.join(here, "web_fakes.mjs"), "utf8").replace(/^export /gm, "");

const install = `
  window.__fakeMidi = {
    kbd: new FakeMidiInput("Fake Keys"),
    synth: new FakeMidiOutput("Fake Synth"),
  };
  window.__fakeMidi.access = new FakeMidiAccess({
    inputs: [window.__fakeMidi.kbd], outputs: [window.__fakeMidi.synth] });
  Object.defineProperty(navigator, "requestMIDIAccess", {
    configurable: true, value: async () => window.__fakeMidi.access });

  window.__fakePads = new FakeGamepads();
  window.__fakePads.events = window;   // where the manager listens
  window.__fakePads.pad = fakeGamepad(0, "Fake Pad");
  window.__fakePads.connect(window.__fakePads.pad);
  Object.defineProperty(navigator, "getGamepads", {
    configurable: true, value: () => window.__fakePads.getGamepads() });
`;

test.beforeEach(async ({ page }) => {
  await page.addInitScript(fakes + "\n" + install);
});

// Boot a client with the subsystems on, collect every inbound message, and
// run `body` against it. Runs in the page.
const IN_PAGE_HARNESS = `
  window.__harness = async (config, body) => {
    const clockwork = new window.Clockwork({ ...config, midi: config.midi ?? true, gamepad: config.gamepad ?? true });
    const inbound = [];
    clockwork.on("in", (msg) => inbound.push(msg));
    const waitFor = (address, ms = 4000) => new Promise((resolve) => {
      const t0 = performance.now();
      const look = () => {
        const hit = inbound.find((m) => m[0] === address);
        if (hit) return resolve(hit);
        if (performance.now() - t0 > ms) return resolve(null);
        setTimeout(look, 10);
      };
      look();
    });
    const until = (pred, ms = 4000) => new Promise((resolve) => {
      const t0 = performance.now();
      const look = () => {
        if (pred()) return resolve(true);
        if (performance.now() - t0 > ms) return resolve(false);
        setTimeout(look, 10);
      };
      look();
    });
    await clockwork.init();
    try { return await body({ clockwork, inbound, waitFor, until }); }
    finally { await clockwork.shutdown(); }
  };
`;

async function run(page, config, body) {
  await boot(page);
  await page.evaluate(IN_PAGE_HARNESS);
  return page.evaluate(([cfg, src]) => window.__harness(cfg, eval(`(${src})`)), [config, body.toString()]);
}

test("the host lists the MIDI ports, on either transport", async ({ page, clockworkConfig }) => {
  const r = await run(page, clockworkConfig, async ({ clockwork, waitFor }) => {
    clockwork.send("/clockwork/midi/ports/list");
    return await waitFor("/clockwork/midi/ports.reply");
  });
  expect(r, "no /clockwork/midi/ports.reply came back").not.toBeNull();
  // <nIn> [name enabled]* <nOut> [name enabled]*: the fake's ports, closed.
  expect(r.slice(1)).toEqual([1, "fake_keys", 0, 1, "fake_synth", 0]);
});

test("a note sent now reaches the port now", async ({ page, clockworkConfig }) => {
  const r = await run(page, clockworkConfig, async ({ clockwork, until }) => {
    clockwork.send("/clockwork/midi/out/enable", "fake_synth", 1);
    clockwork.send("/clockwork/midi/out/note_on", "fake_synth", 1, 60, 100);
    const got = await until(() => window.__fakeMidi.synth.sent.length === 1);
    return { got, sent: window.__fakeMidi.synth.sent };
  });
  expect(r.got, "the note never reached the port").toBe(true);
  expect(r.sent[0].bytes).toEqual([0x90, 60, 100]);
  expect(r.sent[0].timestamp).toBeUndefined();
});

test("a scheduled note reaches the port early, stamped with its moment", async ({ page, clockworkConfig }) => {
  // The contract the whole hop exists for. The scheduler fires the note on
  // the audio thread at the block that holds its time; the forward carries
  // that time to the front; the front hands the browser the bytes AND the
  // time. So the port sees the send BEFORE the moment, with the moment as its
  // timestamp — and the browser, not clockwork, holds it to that moment.
  const r = await run(page, clockworkConfig, async ({ clockwork, until }) => {
    clockwork.send("/clockwork/midi/out/enable", "*", 1);
    const leadS = 0.4;
    const dueNtp = window.Clockwork.osc.ntpNow() + leadS;
    const duePerfMs = performance.now() + leadS * 1000;
    const inner = window.Clockwork.osc.encodeMessage("/clockwork/midi/out/note_on", ["fake_synth", 1, 64, 90]);
    clockwork.send("/clockwork/schedule", { type: "double", value: dueNtp }, { type: "blob", value: inner });
    const got = await until(() => window.__fakeMidi.synth.sent.length === 1, 3000);
    return { got, sent: window.__fakeMidi.synth.sent, sentAt: window.__fakeMidi.synth.sentAt, duePerfMs };
  });
  expect(r.got, "the scheduled note never reached the port").toBe(true);
  expect(r.sent[0].bytes).toEqual([0x90, 64, 90]);
  const { timestamp } = r.sent[0];
  expect(timestamp, "the send carried no timestamp: the time was lost on the hop").toBeDefined();
  // Within a few ms of the moment asked for...
  expect(Math.abs(timestamp - r.duePerfMs), `stamped ${timestamp - r.duePerfMs} ms off`).toBeLessThan(30);
  // ...and handed over before it, for the browser to hold.
  expect(r.sentAt[0], "the send was made after its moment, not held to it").toBeLessThan(timestamp);
});

test("a note the guest sends itself reaches the port stamped with its moment", async ({ page, clockworkConfig }) => {
  // The self-directed guest's path: never through ingress, never through
  // the scheduler. The dummy opens a sink through DspHost and sends from
  // dsp_process with its own time; the engine's sink endpoint on the web is
  // the host, so the bytes come out over the egress with that time, and the
  // front hands them to the port — which it opens on demand, since no
  // client enabled it — with the moment as the timestamp.
  const r = await run(page, clockworkConfig, async ({ clockwork, waitFor, until }) => {
    clockwork.send("/dummy/sink/open", "fake_synth", { type: "int", value: 1 }, { type: "int", value: 0 });
    const opened = await waitFor("/dummy/sink/opened");
    const sink = opened?.[1] ?? 0;
    const duePerfMs = performance.now() + 300;
    clockwork.send("/dummy/sink/send", { type: "int", value: sink },
      { type: "blob", value: Uint8Array.from([0x90, 0x3c, 0x64]) }, { type: "int", value: 300 });
    const got = await until(() => window.__fakeMidi.synth.sent.length === 1, 3000);
    // The audio path's own latency, for the budget below: a headless runner's
    // output latency is nothing like a machine with an interface, and the gap
    // this test measures is mostly made of it.
    let audio = null;
    try { audio = clockwork.getSystemReport()?.audio ?? null; } catch { /* report unavailable */ }
    return { sink, got, sent: window.__fakeMidi.synth.sent, sentAt: window.__fakeMidi.synth.sentAt, duePerfMs, audio };
  });
  expect(r.sink, "the guest's sink did not open").toBeGreaterThan(0);
  expect(r.got, "the guest's note never reached the port").toBe(true);
  expect(r.sent[0].bytes).toEqual([0x90, 0x3c, 0x64]);
  const { timestamp } = r.sent[0];
  expect(timestamp, "the send carried no timestamp: the time was lost on the hop").toBeDefined();
  // The guest dated it from its block's start, which is within a block of
  // when the client sent; the browser is handed that moment.
  //
  // The threshold stays at 40. It has been missed by fractions of a
  // millisecond on a CI runner — 41.4 and 40.3 in consecutive runs — which
  // looks like a systematic offset sitting on the limit rather than jitter
  // scattering across it. The audio numbers below are reported so the NEXT
  // failure says what that offset is made of: the stamp is on the output
  // timeline, so if it is the audio path's own latency then baseLatency +
  // outputLatency should account for most of it, and the budget could then be
  // derived from them honestly. Until a run actually prints them that is a
  // hypothesis, and widening the limit now would bury the evidence for it.
  // Web Audio reports both latencies in SECONDS.
  const audio = r.audio ?? {};
  const latencyMs = ((audio.baseLatency ?? 0) + (audio.outputLatency ?? 0)) * 1000;
  const blockMs = audio.sampleRate ? (128 / audio.sampleRate) * 1000 : 128 / 48;
  expect(
    Math.abs(timestamp - r.duePerfMs),
    `stamped ${timestamp - r.duePerfMs} ms off. ` +
      `baseLatency ${audio.baseLatency}s + outputLatency ${audio.outputLatency}s ` +
      `= ${latencyMs.toFixed(2)} ms; block ${blockMs.toFixed(2)} ms at ${audio.sampleRate} Hz. ` +
      `If that latency accounts for the offset, the limit is measuring the box's ` +
      `output buffer rather than clockwork. Null or 0 means the browser would not ` +
      `report them, which is its own finding.`,
  ).toBeLessThan(40);
  expect(r.sentAt[0], "the send was made after its moment, not held to it").toBeLessThan(timestamp);
});

test("what the keyboard plays reaches the client once subscribed, and the guest that asked", async ({ page, clockworkConfig }) => {
  // The event goes INTO the engine from the main thread, carrying the moment
  // it arrived, and one audio-thread route sends it out to the subscribed
  // client and hands it to the guest — the same route a native port's event
  // takes, so neither can tell which host produced it. The dummy guest asks
  // for events (DspInfo::wants_events) and reports the last one it heard.
  const r = await run(page, clockworkConfig, async ({ clockwork, waitFor, inbound }) => {
    clockwork.send("/clockwork/midi/in/enable", "fake_keys", 1);
    clockwork.send("/clockwork/midi/notify/subscribe", 41);
    const ack = await waitFor("/clockwork/midi/notify/subscribe.reply");
    const playedAt = performance.now();
    window.__fakeMidi.kbd.receive([0x90, 62, 80], playedAt);
    const note = await waitFor("/clockwork/midi/in/note_on");
    clockwork.send("/dummy/events/get");
    const heard = await waitFor("/dummy/events");
    return { ack, note, heard, inbound, playedAt, timeOrigin: performance.timeOrigin };
  });
  expect(r.ack, "the subscribe was not acked").not.toBeNull();
  expect(r.ack[1]).toBe(41);
  expect(r.note, "the note never reached the client").not.toBeNull();
  expect(r.note.slice(0, 5)).toEqual(["/clockwork/midi/in/note_on", "fake_keys", 1, 62, 80]);
  // The trailing timetag is the moment it was played, in NTP seconds.
  const arrivedUnixMs = (r.note[5] - 2208988800) * 1000;
  expect(Math.abs(arrivedUnixMs - (r.timeOrigin + r.playedAt))).toBeLessThan(2);
  // The guest heard it too.
  expect(r.heard, "the guest was never asked").not.toBeNull();
  expect(r.heard[1]).toBe("/clockwork/midi/in/note_on");
  expect(r.heard[2]).toBeGreaterThanOrEqual(1);
  // The forwarded verbs themselves never surface as inbound: the front took them.
  expect(r.inbound.map((m) => m[0])).not.toContain("/clockwork/midi/in/enable");
});

test("an event is not heard by a client that did not subscribe", async ({ page, clockworkConfig }) => {
  const r = await run(page, clockworkConfig, async ({ clockwork, waitFor, until, inbound }) => {
    clockwork.send("/clockwork/midi/in/enable", "fake_keys", 1);
    // The enable is a round trip through the worklet; play once it has opened the port.
    await until(() => clockwork.midi?.portLists().ins.some((r) => r[1]));
    window.__fakeMidi.kbd.receive([0x90, 62, 80]);
    // The guest hears it regardless: subscription is a client's business.
    await new Promise((res) => setTimeout(res, 200));
    clockwork.send("/dummy/events/get");
    const heard = await waitFor("/dummy/events");
    return { heard, addresses: inbound.map((m) => m[0]) };
  });
  expect(r.addresses).not.toContain("/clockwork/midi/in/note_on");
  expect(r.heard[1]).toBe("/clockwork/midi/in/note_on");
});

test("a gamepad press reaches the client, and rumble reaches the pad", async ({ page, clockworkConfig }) => {
  const r = await run(page, clockworkConfig, async ({ clockwork, waitFor, until }) => {
    clockwork.send("/clockwork/gamepad/notify/subscribe", 52);
    const devices = await waitFor("/clockwork/gamepad/devices.reply");
    const handle = devices?.[2];
    const pad = window.__fakePads.pad;
    pad.buttons[0].pressed = true; pad.buttons[0].value = 1;
    const press = await waitFor("/clockwork/gamepad/in/button");
    clockwork.send("/dummy/events/get");
    const heard = await waitFor("/dummy/events");
    clockwork.send("/clockwork/gamepad/out/rumble", handle, 1.0, 0.5, 200);
    const rumbled = await until(() => pad.vibrationActuator.effects.length === 1);
    return { devices, handle, press, heard, rumbled, effect: pad.vibrationActuator.effects[0] ?? null };
  });
  expect(r.devices, "no devices reply").not.toBeNull();
  expect(r.press, "the press never reached the client").not.toBeNull();
  expect(r.press[1]).toBe(r.handle);
  expect(r.press[3]).toBe(1);
  expect(r.press.length, "the press carried no timetag").toBe(6);
  expect(r.heard[1], "the guest did not hear the press").toBe("/clockwork/gamepad/in/button");
  expect(r.rumbled, "rumble never reached the pad").toBe(true);
  expect(r.effect.strongMagnitude).toBe(1);
  expect(r.effect.duration).toBe(200);
});

test("with MIDI off, a MIDI verb is refused by name; an unknown verb always is", async ({ page, clockworkConfig }) => {
  const r = await run(page, { ...clockworkConfig, midi: false, gamepad: false }, async ({ clockwork, waitFor, inbound }) => {
    clockwork.send("/clockwork/midi/ports/list");
    const first = await waitFor("/clockwork/error");
    clockwork.send("/clockwork/no/such/verb");
    const errors = () => inbound.filter((m) => m[0] === "/clockwork/error");
    await new Promise((res) => { const look = () => errors().length >= 2 ? res() : setTimeout(look, 10); look(); });
    return errors();
  });
  expect(r[0][1]).toBe("/clockwork/midi/ports/list");
  expect(r[0][2]).toContain("midi: true");
  expect(r[1][1]).toBe("/clockwork/no/such/verb");
  expect(r[1][2]).toBe("unknown clockwork verb");
});

test("ping is still answered on the audio thread, not by the host", async ({ page, clockworkConfig }) => {
  // Liveness does not move to the main thread with the rest: a front that
  // has gone away must not take ping with it.
  const r = await run(page, clockworkConfig, async ({ clockwork, waitFor }) => {
    clockwork.send("/clockwork/ping", 9);
    return await waitFor("/clockwork/pong");
  });
  expect(r).toEqual(["/clockwork/pong", 9]);
});
