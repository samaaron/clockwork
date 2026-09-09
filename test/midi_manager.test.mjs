// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * midi_manager.test.mjs — the web MIDI boundary, under node, against the
 * real Rust core (dist/midi, built by scripts/build-web-subsystems.sh) and a
 * fake Web MIDI (web_fakes.mjs).
 *
 * The contract is the native subsystem's, to the byte: ports are closed
 * until opened, a reply's payload is what rust/clockwork-midi encodes, a
 * send reaches only an open port, and a send with a time reaches the port
 * with that time as a DOMHighResTimeStamp. Web MIDI itself is faked; the
 * protocol work is not.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { MidiManager } from "../js/lib/midi_manager.js";
import * as oscFast from "../js/lib/osc_fast.js";
import { clockworkSys } from "../js/lib/clockwork_sys.js";
import { unixMsToTimetag, timetagToPerfMs } from "../js/lib/timetag.js";
import { FakeMidiAccess, FakeMidiInput, FakeMidiOutput } from "./web_fakes.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = path.join(here, "..", "dist", "wasm", "clockwork_midi_bg.wasm");
const maybe = fs.existsSync(wasmPath) ? test : test.skip;
const wasm = () => fs.readFileSync(wasmPath);

const decode = (bytes) => oscFast.decodePacket(bytes);
const encode = (address, args) => oscFast.copyEncoded(oscFast.encodeMessage(address, args));

// A manager over a fake with one input and two outputs, ports still closed.
async function booted() {
  const kbd = new FakeMidiInput("My Keyboard");
  const synth = new FakeMidiOutput("Synth 1");
  const drums = new FakeMidiOutput("Drum Box");
  const access = new FakeMidiAccess({ inputs: [kbd], outputs: [synth, drums] });
  const m = new MidiManager({ requestAccess: async () => access, wasm: wasm() });
  await m.init();
  return { m, access, kbd, synth, drums };
}

maybe("ports are listed in the native wire form, closed until opened", async () => {
  const { m } = await booted();
  const reply = decode(m.portsReply());
  // /clockwork/midi/ports.reply <nIn> [name enabled]* <nOut> [name enabled]*
  assert.equal(reply[0], clockworkSys("midi/ports.reply"));
  assert.deepEqual(reply.slice(1), [1, "my_keyboard", 0, 2, "synth_1", 0, "drum_box", 0]);
});

maybe("an input is not heard from until it is enabled", async () => {
  const { m, kbd } = await booted();
  const events = [];
  m.onEvent((osc) => events.push(decode(osc)));

  kbd.receive([0x90, 60, 100]);
  assert.deepEqual(events, [], "a closed input was heard");

  m.enable("my_keyboard", true, true);
  kbd.receive([0x90, 60, 100], 1234.5);
  // The fields, then the moment it arrived (the event's own timestamp) as a
  // trailing timetag — what a guest placing the note on its timeline wants.
  assert.equal(events.length, 1);
  assert.deepEqual(events[0].slice(0, 5), [clockworkSys("midi/in/note_on"), "my_keyboard", 1, 60, 100]);
  assert.equal(events[0].length, 6);
  const arrivedPerfMs = (events[0][5] - 2208988800) * 1000 - performance.timeOrigin;
  assert.ok(Math.abs(arrivedPerfMs - 1234.5) < 2, `arrival ${arrivedPerfMs}`);

  m.enable("my_keyboard", true, false);
  kbd.receive([0x80, 60, 0]);
  assert.equal(events.length, 1, "a closed-again input was heard");
});

maybe("the structured fast path carries the same fields with no OSC round trip", async () => {
  const { m, kbd } = await booted();
  const fields = [];
  m.onMessage((f) => fields.push(f));
  m.enable("*", true, true);
  kbd.receive([0xb0, 7, 127]);
  assert.deepEqual(fields, [["control_change", "my_keyboard", 1, 7, 127]]);
});

maybe("a send reaches only an open output, and '*' reaches every open one", async () => {
  const { m, synth, drums } = await booted();
  const noteOn = encode(clockworkSys("midi/out/note_on"), ["synth_1", 1, 60, 100]);

  assert.equal(m.sendOut(noteOn), true);
  assert.deepEqual(synth.sent, [], "a closed output was sent to");

  m.enable("synth_1", false, true);
  m.sendOut(noteOn);
  assert.deepEqual(synth.sent, [{ bytes: [0x90, 60, 100], timestamp: undefined }]);

  const all = encode(clockworkSys("midi/out/note_off"), ["*", 1, 60, 0]);
  m.sendOut(all);
  assert.equal(synth.sent.length, 2);
  assert.deepEqual(drums.sent, [], "'*' reached a closed output");

  m.enable("*", false, true);
  m.sendOut(all);
  assert.deepEqual(drums.sent, [{ bytes: [0x80, 60, 0], timestamp: undefined }]);
});

maybe("a send from a guest's sink opens the port on demand, once, and says so", async () => {
  const { m, synth, drums } = await booted();
  const pushes = [];
  m.onPorts((osc) => pushes.push(decode(osc)));

  m.sendFromSink("synth_1", Uint8Array.from([0x90, 60, 100]), 1234.5);
  assert.deepEqual(synth.sent, [{ bytes: [0x90, 60, 100], timestamp: 1234.5 }]);
  assert.deepEqual(drums.sent, []);
  // Opened on first use, and the ports push shows it. Not pushed again for
  // the next send: the port was already open.
  assert.equal(pushes.length, 1);
  assert.deepEqual(pushes[0].slice(1), [1, "my_keyboard", 0, 2, "synth_1", 1, "drum_box", 0]);
  m.sendFromSink("synth_1", Uint8Array.from([0x80, 60, 0]), undefined);
  assert.equal(pushes.length, 1);
  assert.equal(synth.sent.length, 2);

  // "*" opens every output.
  m.sendFromSink("*", Uint8Array.from([0xf8]), undefined);
  assert.equal(drums.sent.length, 1);
  assert.equal(pushes.length, 2);

  // A port that is not there is not opened and not sent to.
  m.sendFromSink("no_such_port", Uint8Array.from([0xf8]), undefined);
  assert.equal(pushes.length, 2);
});

maybe("a timed send reaches the port with its time as a DOMHighResTimeStamp", async () => {
  const { m, synth } = await booted();
  m.enable("synth_1", false, true);

  // The verb's own trailing timetag: half a second from now, in NTP.
  const dueUnixMs = Date.now() + 500;
  const tt = unixMsToTimetag(dueUnixMs);
  const timed = encode(clockworkSys("midi/out/note_on"),
    ["synth_1", 1, 64, 90, { type: "timetag", value: [Number(tt >> 32n), Number(tt & 0xffffffffn)] }]);
  m.sendOut(timed);
  const expect = timetagToPerfMs(tt);
  assert.equal(synth.sent.length, 1);
  assert.ok(Math.abs(synth.sent[0].timestamp - expect) < 1, `timestamp ${synth.sent[0].timestamp} vs ${expect}`);

  // An explicit override — the host front carrying a scheduled verb's
  // time — wins over the verb's own.
  m.sendOut(timed, 1234.5);
  assert.equal(synth.sent[1].timestamp, 1234.5);
});

maybe("channel 0 fans a message out to all sixteen channels in one send", async () => {
  const { m, synth } = await booted();
  m.enable("synth_1", false, true);
  m.sendOut(encode(clockworkSys("midi/out/note_on"), ["synth_1", 0, 60, 100]));
  assert.equal(synth.sent.length, 1);
  assert.equal(synth.sent[0].bytes.length, 16 * 3);
  assert.equal(synth.sent[0].bytes[0], 0x90);
  assert.equal(synth.sent[0].bytes[45], 0x9f);
});

maybe("a verb that is not a send is not sent", async () => {
  const { m, synth } = await booted();
  m.enable("*", false, true);
  assert.equal(m.sendOut(encode(clockworkSys("midi/ports/list"), [])), false);
  assert.equal(m.sendOut(encode("/dummy/ping", [])), false);
  assert.deepEqual(synth.sent, []);
});

maybe("ports are pushed on an enable, and on a hotplug only when the list changed", async () => {
  const { m, access } = await booted();
  const pushes = [];
  m.onPorts((osc) => pushes.push(decode(osc)));

  m.enable("synth_1", false, true);
  assert.equal(pushes.length, 1);
  assert.equal(pushes[0][0], clockworkSys("midi/ports"));
  assert.deepEqual(pushes[0].slice(1), [1, "my_keyboard", 0, 2, "synth_1", 1, "drum_box", 0]);

  // A statechange that changes nothing is not a push (the browser fires them
  // for transient transitions).
  access.onstatechange({});
  assert.equal(pushes.length, 1);

  const pad = new FakeMidiInput("Pad Controller");
  access.connect(pad);
  assert.equal(pushes.length, 2);
  assert.deepEqual(pushes[1].slice(1), [2, "my_keyboard", 0, "pad_controller", 0, 2, "synth_1", 1, "drum_box", 0]);

  access.disconnect(pad);
  assert.equal(pushes.length, 3);

  // A refresh is a request for the list: always a push.
  m.refresh();
  assert.equal(pushes.length, 4);
});

maybe("a port enabled before it is plugged in opens when it appears", async () => {
  const { m, access } = await booted();
  const events = [];
  m.onEvent((osc) => events.push(decode(osc)));
  m.enable("pad_controller", true, true);
  const pad = new FakeMidiInput("Pad Controller");
  access.connect(pad);
  pad.receive([0x90, 36, 127]);
  assert.equal(events.length, 1);
  assert.equal(events[0][1], "pad_controller");
});

maybe("clock pulses feed the tempo estimate and never surface as events", async () => {
  const { m, kbd } = await booted();
  const events = [];
  const tempos = [];
  m.onEvent((osc) => events.push(osc));
  m.onTempo((port, bpm) => tempos.push([port, bpm]));
  m.enable("*", true, true);

  // 120 BPM: 24 pulses a beat, 500 ms a beat → one every 20.833 ms.
  for (let i = 0; i < 96; i++) kbd.receive([0xf8], i * (500 / 24));
  assert.deepEqual(events, [], "a clock pulse surfaced as an event");
  assert.ok(tempos.length > 0, "no tempo was estimated");
  const [port, bpm] = tempos[tempos.length - 1];
  assert.equal(port, "my_keyboard");
  assert.ok(Math.abs(bpm - 120) < 1, `estimated ${bpm}`);

  // clock/sync 0 mutes the feed.
  m.clockSync("my_keyboard", false);
  const before = tempos.length;
  for (let i = 0; i < 96; i++) kbd.receive([0xf8], 5000 + i * (500 / 24));
  assert.equal(tempos.length, before);
});

maybe("a clock tick is one 0xF8 to the open outputs", async () => {
  const { m, synth, drums } = await booted();
  m.enable("drum_box", false, true);
  m.tick("*");
  assert.deepEqual(synth.sent, []);
  assert.deepEqual(drums.sent, [{ bytes: [0xf8], timestamp: undefined }]);
});

maybe("dispose closes everything", async () => {
  const { m, kbd, synth } = await booted();
  const events = [];
  m.onEvent((osc) => events.push(osc));
  m.enable("*", true, true);
  m.enable("*", false, true);
  m.dispose();
  kbd.receive([0x90, 60, 100]);
  assert.deepEqual(events, []);
  m.sendOut(encode(clockworkSys("midi/out/note_on"), ["*", 1, 60, 100]));
  assert.deepEqual(synth.sent, []);
});
