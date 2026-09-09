// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
// GamepadManager — the web gamepad boundary (main thread). Owns Gamepad API I/O
// (polling navigator.getGamepads()) and drives the shared Rust core (compiled
// to wasm) for all protocol logic: per-pad diffing with the same deadzone,
// quantisation and press hysteresis as native, canonical button/axis names,
// /clockwork/gamepad/in/* OSC encoding, /clockwork/gamepad/out/* decoding, the
// devices payloads, and name normalisation. This is the web counterpart of the
// native GamepadControl + Rust subsystem — the /clockwork/gamepad/* OSC
// contract is identical, down to the bytes of a reply.
//
// The Gamepad API is poll-only and main-thread only, so this runs alongside
// the Clockwork JS, not in the AudioWorklet. Polling uses setInterval, not
// requestAnimationFrame: rAF throttles in background tabs, which would mute a
// controller the moment the tab loses focus — wrong for a music engine. The
// wasm diffing means the poll cadence never shows up on the wire; only real
// changes do.
//
// PADS ARE ENABLED BY DEFAULT, as they are natively: /clockwork/gamepad/enable
// mutes one (or "*", which also sets the default for pads that connect later).
//
// The Gamepad API is INJECTABLE (`getGamepads`, `events`): a test hands in a
// fake with the same shape, and the whole manager runs under node against
// the real wasm core. The default asks the browser.

import { perfMsToTimetag } from "./timetag.js";
import init, {
  assign_handle,
  gamepad_axis_osc_at,
  gamepad_button_osc_at,
  gamepad_devices_osc,
  gamepad_devices_reply_osc,
  gamepad_out_decode,
  WasmPadState,
} from "../../dist/gamepad/clockwork_gamepad.js";

export class GamepadManager {
  /**
   * @param {object} [options]
   * @param {number} [options.pollIntervalMs=8]
   * @param {number} [options.rumbleRefreshMs=4500] how often an active "until
   *   stopped" (or > 5 s) rumble is re-issued — the Gamepad API caps a single
   *   effect at 5 s, so the poll loop renews it just before expiry to match
   *   the native until-stop semantics.
   * @param {() => (Gamepad|null)[]} [options.getGamepads] defaults to
   *   navigator.getGamepads().
   * @param {EventTarget} [options.events] where gamepadconnected /
   *   gamepaddisconnected fire; defaults to window.
   * @param {string|URL|Uint8Array} [options.wasm] where the core's wasm is,
   *   or its bytes; defaults to beside the glue module.
   * @param {() => number} [options.now] performance.now, injectable.
   */
  constructor(options = {}) {
    this._pollIntervalMs = options.pollIntervalMs ?? 8;
    this._rumbleRefreshMs = options.rumbleRefreshMs ?? 4500;
    this._getGamepads = options.getGamepads
      ?? (() => {
        if (typeof navigator === "undefined" || !navigator.getGamepads)
          throw new Error("Gamepad API unavailable");
        return navigator.getGamepads();
      });
    this._events = options.events ?? (typeof window !== "undefined" ? window : null);
    this._wasm = options.wasm;
    this._now = options.now ?? (() => performance.now());
    this._timer = null;
    this._pads = new Map(); // Gamepad.index -> { id, handle, enabled, state: WasmPadState, ... }
    this._defaultEnabled = true; // what a pad that connects later gets
    this._onEvent = null; // (Uint8Array osc) => void  — /clockwork/gamepad/in/* OSC packet
    this._onMessage = null; // (Array [kind, pad, name, ...]) => void  — structured
    this._onDevices = null; // (Uint8Array osc) => void  — /clockwork/gamepad/devices push
    this._lastDevicesKey = null; // last emitted pad list, to suppress no-op pushes
    this._onChange = () => this._refresh(false);
  }

  // Load the wasm core and start polling. Browsers only surface a pad after a
  // user gesture (typically the first button press), so an empty initial list
  // is normal; connect/disconnect events + polling pick pads up as they appear.
  async init() {
    await init(this._wasm !== undefined ? { module_or_path: this._wasm } : undefined);
    this._events?.addEventListener("gamepadconnected", this._onChange);
    this._events?.addEventListener("gamepaddisconnected", this._onChange);
    this._refresh(false);
    this._timer = setInterval(() => this.poll(), this._pollIntervalMs);
    return this;
  }

  dispose() {
    if (this._timer) clearInterval(this._timer);
    this._timer = null;
    this._events?.removeEventListener("gamepadconnected", this._onChange);
    this._events?.removeEventListener("gamepaddisconnected", this._onChange);
    this._pads.clear();
  }

  onEvent(cb) { this._onEvent = cb; }
  // Structured inbound events: cb receives ["button", pad, name, pressed01,
  // value] or ["axis", pad, name, value] with no OSC encode/decode round-trip.
  // Takes precedence over onEvent when both are set.
  onMessage(cb) { this._onMessage = cb; }
  // A /clockwork/gamepad/devices push, as bytes: on a connect, disconnect or
  // enable change.
  onDevices(cb) { this._onDevices = cb; }

  // ── Devices ──────────────────────────────────────────────────────────────

  // [[handle, enabled], ...] in registry order.
  deviceRows() {
    return [...this._pads.values()].map((e) => [e.handle, e.enabled]);
  }

  // The /clockwork/gamepad/devices.reply packet for the current state.
  devicesReply() {
    const rows = this.deviceRows();
    return gamepad_devices_reply_osc(rows.map((r) => r[0]), Uint8Array.from(rows, (r) => (r[1] ? 1 : 0)));
  }

  _devicesPush() {
    const rows = this.deviceRows();
    return gamepad_devices_osc(rows.map((r) => r[0]), Uint8Array.from(rows, (r) => (r[1] ? 1 : 0)));
  }

  // Mute (or unmute) a pad's events — "*" for every pad, and then also the
  // default for pads that connect later. Pushes /clockwork/gamepad/devices.
  enable(pad, enabled) {
    if (pad === "*") {
      this._defaultEnabled = enabled;
      for (const entry of this._pads.values()) entry.enabled = enabled;
    } else {
      for (const entry of this._pads.values()) {
        if (entry.handle === pad) entry.enabled = enabled;
      }
    }
    this._push(true);
  }

  // Re-enumerate and push /clockwork/gamepad/devices whether or not anything
  // changed — a refresh is a request for the list.
  refresh() {
    this._refresh(true);
  }

  _refresh(force) {
    const pads = this._getGamepads();
    // Drop vanished/replaced pads first so their handles free up…
    for (const [index, entry] of [...this._pads]) {
      const pad = pads[index];
      if (!pad || pad.id !== entry.id) this._pads.delete(index);
    }
    // …then register new arrivals. Handle assignment (normalise + _2/_3
    // dedup) is the shared Rust rule, so a handle means the same device on
    // web and native.
    const taken = [...this._pads.values()].map((e) => e.handle);
    for (const pad of pads) {
      if (!pad || this._pads.has(pad.index)) continue;
      const handle = assign_handle(pad.id, taken);
      taken.push(handle);
      this._pads.set(pad.index, {
        id: pad.id,
        handle,
        enabled: this._defaultEnabled,
        state: new WasmPadState(pad.mapping === "standard"),
        // Poll-snapshot buffers, reused every tick (element counts are fixed
        // for the life of a pad) so polling doesn't churn the GC.
        pressed: new Uint8Array(pad.buttons.length),
        values: new Float64Array(pad.buttons.length),
        axes: new Float64Array(pad.axes.length),
      });
    }
    this._push(force);
  }

  // Browsers can fire connect/disconnect for transient transitions; only
  // push when the pad list actually changed, mirroring the native
  // "broadcast only on change" behaviour — unless asked outright.
  _push(force) {
    const key = JSON.stringify(this.deviceRows());
    if (!force && key === this._lastDevicesKey) return;
    this._lastDevicesKey = key;
    if (this._onDevices) this._onDevices(this._devicesPush());
  }

  // One poll: what changed since the last one, as events. Runs on the
  // interval; callable by hand, which is how a test drives it.
  poll() {
    const pads = this._getGamepads();
    for (const pad of pads) {
      if (!pad) continue;
      let entry = this._pads.get(pad.index);
      const buttons = pad.buttons;
      const axes = pad.axes;
      if (
        !entry ||
        entry.id !== pad.id ||
        // Element counts are fixed for a connection, so a mismatch means the
        // entry is stale; re-register rather than silently truncating (which
        // would leave the extra inputs permanently dead).
        buttons.length !== entry.pressed.length ||
        axes.length !== entry.axes.length
      ) {
        this._pads.delete(pad.index);
        this._refresh(false); // pad appeared (or slot/shape changed) between events
        entry = this._pads.get(pad.index);
        if (!entry) continue;
      }
      for (let i = 0; i < buttons.length; i++) {
        const b = buttons[i];
        entry.pressed[i] = b.pressed ? 1 : 0;
        entry.values[i] = b.value;
      }
      for (let i = 0; i < axes.length; i++) entry.axes[i] = axes[i];
      // No-change ticks (the overwhelmingly common case) return undefined —
      // no per-tick array materialised. The diff runs even for a muted pad,
      // so unmuting does not replay every change made while it was muted.
      const events = entry.state.update(entry.pressed, entry.values, entry.axes);
      if (events && entry.enabled) {
        for (let i = 0; i < events.length; i += 4) {
          this._emit(entry.handle, events[i], events[i + 1], events[i + 2], events[i + 3]);
        }
      }
      this._refreshRumble(entry, pad);
    }
    // A pad unplugged mid-poll surfaces as a null slot before the
    // gamepaddisconnected event lands; sweep so the devices push is prompt.
    for (const index of this._pads.keys()) {
      if (!pads[index]) {
        this._refresh(false);
        break;
      }
    }
  }

  _emit(pad, kind, name, a, b) {
    // Prefer the structured fast path; fall back to OSC bytes for consumers
    // (e.g. the native-shaped engine ingress) that want the wire form.
    if (this._onMessage) {
      this._onMessage(kind === "button" ? [kind, pad, name, a, b] : [kind, pad, name, a]);
      return;
    }
    if (!this._onEvent) return;
    // The moment the change was seen, as a trailing timetag (see
    // MidiManager): the poll that saw it, which is as close as the API gets.
    const when = perfMsToTimetag(this._now());
    const osc =
      kind === "button"
        ? gamepad_button_osc_at(pad, name, a, b, when)
        : gamepad_axis_osc_at(pad, name, a, when);
    this._onEvent(osc);
  }

  // Drive rumble from a /clockwork/gamepad/out/* OSC packet (rumble / rumble_stop).
  // Best-effort: pads without a vibrationActuator are skipped. The spec caps a
  // single effect at 5 s, so a rumble outliving the cap (durationMs <= 0 =
  // "until stop", or any longer duration) is renewed from the poll loop —
  // matching the native until-stop semantics. Returns false for a packet
  // that is not an out verb.
  sendOut(oscBytes) {
    const cmd = gamepad_out_decode(oscBytes);
    if (!cmd) return false;
    const [verb, target] = cmd;
    const pads = this._getGamepads();
    for (const [index, entry] of this._pads) {
      if (target !== "*" && entry.handle !== target) continue;
      const pad = pads[index];
      // The registry can be a poll-tick stale: a different pad may have
      // reused the slot — never rumble hardware the handle doesn't name.
      if (!pad || pad.id !== entry.id) continue;
      const actuator = pad.vibrationActuator;
      if (!actuator) continue;
      if (verb === "rumble") {
        const [, , strong, weak, durationMs] = cmd;
        const now = this._now();
        actuator.playEffect("dual-rumble", {
          strongMagnitude: strong,
          weakMagnitude: weak,
          duration: durationMs > 0 ? Math.min(durationMs, 5000) : 5000,
        });
        entry.rumble = {
          strong,
          weak,
          until: durationMs > 0 ? now + durationMs : Infinity,
          nextPlay: now + this._rumbleRefreshMs,
        };
      } else if (verb === "rumble_stop") {
        if (actuator.reset) actuator.reset();
        delete entry.rumble;
      }
    }
    return true;
  }

  // Renew an active long-running rumble before the API's 5 s effect ceiling
  // cuts it off. Called every poll tick for each live pad.
  _refreshRumble(entry, pad) {
    const r = entry.rumble;
    if (!r) return;
    const now = this._now();
    if (now >= r.until) {
      delete entry.rumble; // the final (remaining-duration) effect ends itself
      return;
    }
    if (now < r.nextPlay) return;
    pad.vibrationActuator?.playEffect("dual-rumble", {
      strongMagnitude: r.strong,
      weakMagnitude: r.weak,
      duration: Math.min(r.until - now, 5000),
    });
    r.nextPlay = now + this._rumbleRefreshMs;
  }
}
