// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * gamepad_manager.test.mjs — the web gamepad boundary, under node, against
 * the real Rust core (dist/gamepad, built by scripts/build-web-subsystems.sh)
 * and a fake Gamepad API (web_fakes.mjs).
 *
 * The contract is the native subsystem's: pads are enabled by default and a
 * muted one is diffed but not heard, a devices payload is what
 * rust/clockwork-gamepad encodes, a button or axis change is one event and
 * a no-change poll is none, and rumble reaches the actuator the handle
 * names. The Gamepad API is faked; the diffing and the names are not.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { GamepadManager } from "../js/lib/gamepad_manager.js";
import * as oscFast from "../js/lib/osc_fast.js";
import { clockworkSys } from "../js/lib/clockwork_sys.js";
import { FakeGamepads, fakeGamepad } from "./web_fakes.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const wasmPath = path.join(here, "..", "dist", "wasm", "clockwork_gamepad_bg.wasm");
const maybe = fs.existsSync(wasmPath) ? test : test.skip;
const wasm = () => fs.readFileSync(wasmPath);

const decode = (bytes) => oscFast.decodePacket(bytes);
const encode = (address, args) => oscFast.copyEncoded(oscFast.encodeMessage(address, args));

// A manager over a fake with one standard pad connected. Polling is by hand
// (poll()), so no interval runs under the test.
async function booted() {
  const api = new FakeGamepads();
  const pad = fakeGamepad(0, "Xbox Wireless Controller (Vendor: 045e)");
  api.connect(pad);
  let now = 1000;
  const m = new GamepadManager({
    getGamepads: () => api.getGamepads(),
    events: api.events,
    wasm: wasm(),
    now: () => now,
    pollIntervalMs: 1e9,   // the test polls
  });
  await m.init();
  return { m, api, pad, tick: (ms) => { now += ms; } };
}

maybe("devices are listed in the native wire form, enabled by default", async () => {
  const { m } = await booted();
  const reply = decode(m.devicesReply());
  assert.equal(reply[0], clockworkSys("gamepad/devices.reply"));
  assert.equal(reply[1], 1);
  assert.equal(typeof reply[2], "string");
  assert.equal(reply[3], 1);
  m.dispose();
});

maybe("a press is one button event; holding it is none; a release is one more", async () => {
  const { m, pad } = await booted();
  const events = [];
  m.onEvent((osc) => events.push(decode(osc)));
  const [handle] = m.deviceRows()[0];

  m.poll();
  assert.deepEqual(events, [], "an idle pad produced an event");

  pad.buttons[0].pressed = true; pad.buttons[0].value = 1;
  m.poll();
  m.poll();
  assert.equal(events.length, 1, "a held button was reported more than once");
  assert.equal(events[0][0], clockworkSys("gamepad/in/button"));
  assert.equal(events[0][1], handle);
  assert.equal(typeof events[0][2], "string");   // the canonical name
  assert.equal(events[0][3], 1);
  // ...value, then the moment it was seen as a trailing timetag.
  assert.equal(events[0].length, 6);
  assert.equal(typeof events[0][5], "number");

  pad.buttons[0].pressed = false; pad.buttons[0].value = 0;
  m.poll();
  assert.equal(events.length, 2);
  assert.equal(events[1][3], 0);
  m.dispose();
});

maybe("an axis move is one event, and the deadzone swallows a twitch", async () => {
  const { m, pad } = await booted();
  const events = [];
  m.onEvent((osc) => events.push(decode(osc)));

  pad.axes[0] = 0.01;   // inside any sane deadzone
  m.poll();
  assert.deepEqual(events, [], "a twitch inside the deadzone was reported");

  pad.axes[0] = 0.8;
  m.poll();
  assert.equal(events.length, 1);
  assert.equal(events[0][0], clockworkSys("gamepad/in/axis"));
  assert.ok(Math.abs(events[0][3] - 0.8) < 0.05, `axis value ${events[0][3]}`);
  m.dispose();
});

maybe("the structured fast path carries the same fields", async () => {
  const { m, pad } = await booted();
  const fields = [];
  m.onMessage((f) => fields.push(f));
  pad.buttons[1].pressed = true; pad.buttons[1].value = 1;
  m.poll();
  assert.equal(fields.length, 1);
  assert.equal(fields[0][0], "button");
  assert.equal(fields[0][3], 1);
  m.dispose();
});

maybe("a muted pad is diffed but not heard, and the mute shows in the devices push", async () => {
  const { m, pad } = await booted();
  const events = [];
  const pushes = [];
  m.onEvent((osc) => events.push(decode(osc)));
  m.onDevices((osc) => pushes.push(decode(osc)));
  const [handle] = m.deviceRows()[0];

  m.enable(handle, false);
  assert.equal(pushes.length, 1);
  assert.equal(pushes[0][0], clockworkSys("gamepad/devices"));
  assert.equal(pushes[0][3], 0);

  pad.buttons[0].pressed = true; pad.buttons[0].value = 1;
  m.poll();
  assert.deepEqual(events, [], "a muted pad was heard");

  // Unmuting does not replay what happened while muted: the diff ran.
  m.enable(handle, true);
  m.poll();
  assert.deepEqual(events, []);
  pad.buttons[0].pressed = false; pad.buttons[0].value = 0;
  m.poll();
  assert.equal(events.length, 1);
  m.dispose();
});

maybe("'*' mutes every pad and the ones that connect later", async () => {
  const { m, api } = await booted();
  m.enable("*", false);
  api.connect(fakeGamepad(1, "Second Pad"));
  assert.deepEqual(m.deviceRows().map((r) => r[1]), [false, false]);
  m.dispose();
});

maybe("connect and disconnect push the devices list, once each", async () => {
  const { m, api } = await booted();
  const pushes = [];
  m.onDevices((osc) => pushes.push(decode(osc)));

  api.connect(fakeGamepad(1, "Second Pad"));
  assert.equal(pushes.length, 1);
  assert.equal(pushes[0][1], 2);

  // A transient event that changes nothing is not a push.
  api.events.dispatchEvent(new Event("gamepadconnected"));
  assert.equal(pushes.length, 1);

  api.disconnect(1);
  assert.equal(pushes.length, 2);
  assert.equal(pushes[1][1], 1);

  // A refresh is a request for the list: always a push.
  m.refresh();
  assert.equal(pushes.length, 3);
  m.dispose();
});

maybe("two pads with one name get distinct handles, the native way", async () => {
  const { m, api } = await booted();
  api.connect(fakeGamepad(1, "Xbox Wireless Controller (Vendor: 045e)"));
  const handles = m.deviceRows().map((r) => r[0]);
  assert.equal(handles.length, 2);
  assert.notEqual(handles[0], handles[1]);
  assert.ok(handles[1].endsWith("_2"), handles[1]);
  m.dispose();
});

maybe("rumble reaches the actuator the handle names, and is renewed past the 5 s cap", async () => {
  const { m, pad, api, tick } = await booted();
  const other = fakeGamepad(1, "Other Pad");
  api.connect(other);
  const [handle] = m.deviceRows()[0];

  assert.equal(m.sendOut(encode(clockworkSys("gamepad/out/rumble"), [handle, 0.5, 0.25, 0])), true);
  assert.equal(pad.vibrationActuator.effects.length, 1);
  assert.deepEqual(pad.vibrationActuator.effects[0], {
    type: "dual-rumble", strongMagnitude: 0.5, weakMagnitude: 0.25, duration: 5000 });
  assert.equal(other.vibrationActuator.effects.length, 0, "the wrong pad rumbled");

  // "Until stop" outlives one effect: the poll loop re-issues it.
  tick(4600);
  m.poll();
  assert.equal(pad.vibrationActuator.effects.length, 2);

  m.sendOut(encode(clockworkSys("gamepad/out/rumble_stop"), ["*"]));
  assert.equal(pad.vibrationActuator.resets, 1);
  tick(5000);
  m.poll();
  assert.equal(pad.vibrationActuator.effects.length, 2, "a stopped rumble was renewed");

  // A bounded rumble ends itself.
  m.sendOut(encode(clockworkSys("gamepad/out/rumble"), ["*", 1, 1, 200]));
  assert.equal(pad.vibrationActuator.effects[2].duration, 200);
  assert.equal(other.vibrationActuator.effects.length, 1);
  tick(300);
  m.poll();
  assert.equal(pad.vibrationActuator.effects.length, 3);

  assert.equal(m.sendOut(encode(clockworkSys("gamepad/devices/list"), [])), false);
  m.dispose();
});
