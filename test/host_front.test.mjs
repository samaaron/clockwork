// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * host_front.test.mjs — the far end of clockwork's chain on the web, under
 * node, against the real Rust cores and fake devices (web_fakes.mjs).
 *
 * The worklet forwards every /clockwork/ verb its audio thread does not
 * answer as a bundle — one element, the verb as sent, timetag the call's
 * time — to the client that sent it (src/audio_processor.cpp,
 * clockwork_host_forward_route; pinned natively in test_host_forward.cpp).
 * The front is what takes those. What these pin:
 *
 *   - what the front takes and what it leaves: only that bundle shape;
 *   - a verb nobody answers is refused by name, with a reason;
 *   - MIDI and gamepad verbs are answered when enabled, and refused with the
 *     reason naming the option when not;
 *   - a verb the scheduler fired reaches the port with the scheduled moment
 *     as its timestamp — the time survives the last hop;
 *   - an inbound event is not delivered but sent INTO the engine, as a
 *     native subsystem's callback writes it, and one coming back off the
 *     egress reaches the client only while it is subscribed; a subscribe is
 *     acked with its token, as natively.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  HostFront, REASON_NO_MIDI, REASON_NO_GAMEPAD, REASON_NOT_ON_WEB, REASON_UNKNOWN,
} from "../js/lib/host_front.js";
import * as oscFast from "../js/lib/osc_fast.js";
import { clockworkSys } from "../js/lib/clockwork_sys.js";
import { unixMsToTimetag, timetagToPerfMs } from "../js/lib/timetag.js";
import { FakeMidiAccess, FakeMidiInput, FakeMidiOutput, FakeGamepads, fakeGamepad } from "./web_fakes.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const midiWasm = path.join(here, "..", "dist", "wasm", "clockwork_midi_bg.wasm");
const padWasm = path.join(here, "..", "dist", "wasm", "clockwork_gamepad_bg.wasm");
const maybe = fs.existsSync(midiWasm) && fs.existsSync(padWasm) ? test : test.skip;

const decode = (bytes) => oscFast.decodePacket(bytes);
const encode = (address, args) => oscFast.copyEncoded(oscFast.encodeMessage(address, args));

// Exactly what clockwork_host_forward_route puts on the egress: "#bundle",
// the timetag, the element's size, the element.
function forwarded(element, timetag = 1n) {
  const out = new Uint8Array(20 + element.length);
  out.set([0x23, 0x62, 0x75, 0x6e, 0x64, 0x6c, 0x65, 0]);
  const v = new DataView(out.buffer);
  v.setBigUint64(8, timetag, false);
  v.setUint32(16, element.length, false);
  out.set(element, 20);
  return out;
}

async function front({ midi = false, gamepad = false } = {}) {
  const delivered = [];
  const ingested = [];
  const kbd = new FakeMidiInput("Keys");
  const synth = new FakeMidiOutput("Synth");
  const access = new FakeMidiAccess({ inputs: [kbd], outputs: [synth] });
  const api = new FakeGamepads();
  const pad = fakeGamepad(0, "Pad One");
  api.connect(pad);
  const f = new HostFront({
    midi: midi && { requestAccess: async () => access, wasm: fs.readFileSync(midiWasm) },
    gamepad: gamepad && {
      getGamepads: () => api.getGamepads(), events: api.events,
      wasm: fs.readFileSync(padWasm), pollIntervalMs: 1e9,
    },
    deliver: (bytes) => delivered.push(decode(bytes)),
    ingest: (bytes) => ingested.push(decode(bytes)),
  });
  await f.init();
  return { f, delivered, ingested, kbd, synth, access, api, pad };
}

maybe("the front takes exactly the forwarded shape and nothing else", async () => {
  const { f, delivered } = await front();
  // A plain reply, a guest reply, a bundle for the guest: not the front's.
  assert.equal(f.take(encode(clockworkSys("clock/state.reply"), [1, 2])), false);
  assert.equal(f.take(encode("/dummy/pong", [])), false);
  assert.equal(f.take(forwarded(encode("/dummy/ping", []))), false);
  assert.equal(f.take(new Uint8Array([0x23, 0x62])), false);
  assert.deepEqual(delivered, []);

  // A forwarded verb under the prefix: taken.
  assert.equal(f.take(forwarded(encode(clockworkSys("no/such/verb"), []))), true);
  assert.equal(delivered.length, 1);
  f.dispose();
});

maybe("a verb nobody answers is refused by name", async () => {
  const { f, delivered } = await front();
  f.take(forwarded(encode(clockworkSys("no/such/verb"), [7])));
  assert.deepEqual(delivered, [[clockworkSys("error"), clockworkSys("no/such/verb"), REASON_UNKNOWN]]);
  f.dispose();
});

maybe("MIDI and gamepad verbs are refused with the option's name when not enabled", async () => {
  const { f, delivered } = await front();
  f.take(forwarded(encode(clockworkSys("midi/ports/list"), [])));
  f.take(forwarded(encode(clockworkSys("gamepad/devices/list"), [])));
  assert.deepEqual(delivered, [
    [clockworkSys("error"), clockworkSys("midi/ports/list"), REASON_NO_MIDI],
    [clockworkSys("error"), clockworkSys("gamepad/devices/list"), REASON_NO_GAMEPAD],
  ]);
  f.dispose();
});

maybe("MIDI: ports are listed, opened, and sent to, through the front", async () => {
  const { f, delivered, synth } = await front({ midi: true });
  f.take(forwarded(encode(clockworkSys("midi/ports/list"), [])));
  assert.deepEqual(delivered, [[clockworkSys("midi/ports.reply"), 1, "keys", 0, 1, "synth", 0]]);

  f.take(forwarded(encode(clockworkSys("midi/out/enable"), ["synth", 1])));
  f.take(forwarded(encode(clockworkSys("midi/out/note_on"), ["synth", 1, 60, 100])));
  assert.deepEqual(synth.sent, [{ bytes: [0x90, 60, 100], timestamp: undefined }]);
  // Not subscribed: the enable's ports push was not delivered.
  assert.equal(delivered.length, 1);
  f.dispose();
});

maybe("MIDI: a verb the scheduler fired reaches the port at the scheduled moment", async () => {
  const { f, synth } = await front({ midi: true });
  f.take(forwarded(encode(clockworkSys("midi/out/enable"), ["*", 1])));

  const dueUnixMs = Date.now() + 300;
  const tt = unixMsToTimetag(dueUnixMs);
  f.take(forwarded(encode(clockworkSys("midi/out/note_on"), ["synth", 1, 64, 90]), tt));
  assert.equal(synth.sent.length, 1);
  const expect = timetagToPerfMs(tt);
  assert.ok(Math.abs(synth.sent[0].timestamp - expect) < 1,
    `sent at ${synth.sent[0].timestamp}, scheduled for ${expect}`);
  f.dispose();
});

maybe("MIDI: a guest's own send opens the port on demand and reaches it with its moment", async () => {
  // The engine's sink endpoint on the web is this host: a self-directed
  // guest's send arrives as /clockwork/midi/sink/send with the send's time,
  // to a port no client ever enabled — and opening a sink onto a port opens
  // the port, as it does natively.
  const { f, delivered, synth } = await front({ midi: true });
  const dueUnixMs = Date.now() + 250;
  const tt = unixMsToTimetag(dueUnixMs);
  f.take(forwarded(encode(clockworkSys("midi/sink/send"), ["synth", Uint8Array.from([0x90, 0x3c, 0x64])]), tt));
  assert.equal(synth.sent.length, 1);
  assert.deepEqual(synth.sent[0].bytes, [0x90, 0x3c, 0x64]);
  assert.ok(Math.abs(synth.sent[0].timestamp - timetagToPerfMs(tt)) < 1);

  // The port is open now, and a client asking sees it so.
  f.take(forwarded(encode(clockworkSys("midi/ports/list"), [])));
  assert.deepEqual(delivered[delivered.length - 1], [clockworkSys("midi/ports.reply"), 1, "keys", 0, 1, "synth", 1]);

  // An immediate one, and a malformed one.
  f.take(forwarded(encode(clockworkSys("midi/sink/send"), ["synth", Uint8Array.from([0xf8])])));
  assert.deepEqual(synth.sent[1], { bytes: [0xf8], timestamp: undefined });
  f.take(forwarded(encode(clockworkSys("midi/sink/send"), ["synth"])));
  assert.deepEqual(delivered[delivered.length - 1], [clockworkSys("error"), clockworkSys("midi/sink/send"), "malformed"]);
  f.dispose();
});

maybe("MIDI: an event goes INTO the engine, and comes back to the client only while subscribed", async () => {
  const { f, delivered, ingested, kbd } = await front({ midi: true });
  f.take(forwarded(encode(clockworkSys("midi/in/enable"), ["keys", 1])));
  // The enable's ports push is an event too: into the engine, not to the client.
  assert.equal(ingested.length, 1);
  assert.equal(ingested[0][0], clockworkSys("midi/ports"));

  kbd.receive([0x90, 60, 100]);
  assert.equal(ingested.length, 2, "the note was not sent into the engine");
  assert.deepEqual(ingested[1].slice(0, 5), [clockworkSys("midi/in/note_on"), "keys", 1, 60, 100]);
  assert.equal(ingested[1].length, 6, "the arrival timetag was not carried");
  assert.deepEqual(delivered, [], "an event was delivered to the client directly");

  // What comes back off the egress (clockwork_event_route) is the client's
  // only once subscribed: the front drops it until then, as the native
  // transport would not have sent it.
  const back = encode(clockworkSys("midi/in/note_on"), ["keys", 1, 60, 100]);
  assert.equal(f.take(back), true, "an event reached an unsubscribed client");

  f.take(forwarded(encode(clockworkSys("midi/notify/subscribe"), [41])));
  // The snapshot a new subscriber gets, then the ack with its token.
  assert.equal(delivered[0][0], clockworkSys("midi/ports.reply"));
  assert.deepEqual(delivered[1], [clockworkSys("midi/notify/subscribe.reply"), 41]);
  assert.equal(f.take(back), false, "a subscribed client's event was swallowed");
  assert.equal(f.take(encode(clockworkSys("midi/ports"), [0, 0])), false);
  // A reply is never an event, subscribed or not.
  assert.equal(f.take(encode(clockworkSys("midi/ports.reply"), [0, 0])), false);

  // A token-less subscribe is silent, as natively.
  const n = delivered.length;
  f.take(forwarded(encode(clockworkSys("midi/notify/subscribe"), [])));
  assert.equal(delivered.length, n + 1);   // the snapshot only
  assert.equal(delivered[n][0], clockworkSys("midi/ports.reply"));

  f.take(forwarded(encode(clockworkSys("midi/notify/unsubscribe"), [])));
  assert.equal(f.take(back), true);
  f.dispose();
});

maybe("MIDI: the engine's clock-out verbs are refused as not on the web", async () => {
  const { f, delivered } = await front({ midi: true });
  f.take(forwarded(encode(clockworkSys("midi/clock/beat"), ["*", 500.0])));
  assert.deepEqual(delivered, [[clockworkSys("error"), clockworkSys("midi/clock/beat"), REASON_NOT_ON_WEB]]);
  f.dispose();
});

maybe("gamepad: devices are listed, a press is heard once subscribed, rumble reaches the pad", async () => {
  const { f, delivered, ingested, pad } = await front({ gamepad: true });
  f.take(forwarded(encode(clockworkSys("gamepad/devices/list"), [])));
  assert.equal(delivered[0][0], clockworkSys("gamepad/devices.reply"));
  assert.equal(delivered[0][1], 1);
  const handle = delivered[0][2];

  f.take(forwarded(encode(clockworkSys("gamepad/notify/subscribe"), [52])));
  assert.equal(delivered[1][0], clockworkSys("gamepad/devices.reply"));
  assert.deepEqual(delivered[2], [clockworkSys("gamepad/notify/subscribe.reply"), 52]);

  // A press goes into the engine, stamped; what comes back is the client's.
  pad.buttons[0].pressed = true; pad.buttons[0].value = 1;
  f.gamepad.poll();
  const press = ingested.find((m) => m[0] === clockworkSys("gamepad/in/button"));
  assert.ok(press, "the press was not sent into the engine");
  assert.equal(press[1], handle);
  assert.equal(press.length, 6);
  assert.equal(f.take(encode(clockworkSys("gamepad/in/button"), [handle, "a", 1, 1.0])), false);

  f.take(forwarded(encode(clockworkSys("gamepad/out/rumble"), [handle, 1, 0.5, 100])));
  assert.equal(pad.vibrationActuator.effects.length, 1);

  f.take(forwarded(encode(clockworkSys("gamepad/enable"), [handle, 0])));
  const devices = ingested.filter((m) => m[0] === clockworkSys("gamepad/devices"));
  assert.ok(devices.length >= 1, "the mute's devices push was not sent into the engine");
  assert.equal(devices[devices.length - 1][3], 0);
  f.dispose();
});
