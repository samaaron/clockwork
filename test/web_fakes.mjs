// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * web_fakes.mjs — Web MIDI and the Gamepad API, in the shapes the managers
 * read, with nothing behind them. What a device would do is recorded, so a
 * test can assert on the bytes a port was sent and the timestamp they were
 * sent with — which is the whole of what the web boundary owes a client.
 *
 * Shared by the node suites (midi_manager, gamepad_manager, host_front) and,
 * as source text, by the browser spec, which injects it before the page
 * loads (see midi.spec.mjs). ONE fake, so the two kinds of test cannot
 * disagree about what a device looks like.
 */

// ── Web MIDI ─────────────────────────────────────────────────────────────────

export class FakeMidiInput {
  constructor(name, id = name) {
    this.name = name;
    this.id = id;
    this.type = "input";
    this.onmidimessage = null;
  }
  // What hardware would do: bytes arrive with the event's own timestamp.
  receive(bytes, timeStamp = 0) {
    this.onmidimessage?.({ data: Uint8Array.from(bytes), timeStamp });
  }
}

export class FakeMidiOutput {
  constructor(name, id = name) {
    this.name = name;
    this.id = id;
    this.type = "output";
    this.sent = [];   // { bytes: number[], timestamp: number|undefined }
    this.sentAt = []; // performance.now() at each send, in step with `sent`
  }
  // `sentAt` is when the call was made, so a test can see that a timed send
  // was handed over EARLY and left for the browser to hold to its time.
  send(bytes, timestamp) {
    this.sent.push({ bytes: Array.from(bytes), timestamp });
    this.sentAt.push(typeof performance !== "undefined" ? performance.now() : 0);
  }
}

export class FakeMidiAccess {
  constructor({ inputs = [], outputs = [] } = {}) {
    this.inputs = new Map(inputs.map((p) => [p.id, p]));
    this.outputs = new Map(outputs.map((p) => [p.id, p]));
    this.onstatechange = null;
    this.sysexEnabled = true;
  }
  // Plug or unplug, as the browser would report it.
  connect(port) {
    (port.type === "input" ? this.inputs : this.outputs).set(port.id, port);
    this.onstatechange?.({ port });
  }
  disconnect(port) {
    (port.type === "input" ? this.inputs : this.outputs).delete(port.id);
    this.onstatechange?.({ port });
  }
}

// ── The Gamepad API ──────────────────────────────────────────────────────────

export class FakeActuator {
  constructor() { this.effects = []; this.resets = 0; }
  playEffect(type, params) { this.effects.push({ type, ...params }); return Promise.resolve("complete"); }
  reset() { this.resets += 1; return Promise.resolve("complete"); }
}

// A pad in the W3C "standard" mapping: 17 buttons, 4 axes, all at rest.
export function fakeGamepad(index, id, { buttons = 17, axes = 4, mapping = "standard" } = {}) {
  return {
    index, id, mapping, connected: true,
    buttons: Array.from({ length: buttons }, () => ({ pressed: false, touched: false, value: 0 })),
    axes: new Array(axes).fill(0),
    vibrationActuator: new FakeActuator(),
  };
}

// navigator.getGamepads() returns a sparse, index-keyed array.
export class FakeGamepads {
  constructor() { this.slots = []; this.events = new EventTarget(); }
  getGamepads() { return this.slots; }
  connect(pad) {
    this.slots[pad.index] = pad;
    this.events.dispatchEvent(new Event("gamepadconnected"));
  }
  disconnect(index) {
    this.slots[index] = null;
    this.events.dispatchEvent(new Event("gamepaddisconnected"));
  }
}
