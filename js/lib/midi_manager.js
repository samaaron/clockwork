// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
// MidiManager — the web MIDI boundary (main thread). Owns Web MIDI I/O and drives
// the shared Rust core (compiled to wasm) for all protocol logic: parsing
// inbound bytes to /clockwork/midi/in/* OSC, decoding /clockwork/midi/out/* to raw bytes, name
// normalisation, the ports payloads, and clock-in BPM estimation. This is the
// web counterpart of the native MidiControl + Rust subsystem — the
// /clockwork/midi/* OSC contract is identical, down to the bytes of a reply.
//
// Web MIDI is main-thread only, so this runs alongside the Clockwork JS, not in
// the AudioWorklet. Outbound sends use MIDIOutput.send(bytes, timestamp), so a
// timestamp keeps scheduled MIDI sample-tight via the browser's own scheduler.
//
// PORTS ARE CLOSED UNTIL OPENED, as they are natively: enumeration lists every
// port with enabled = 0, and nothing is heard from an input or sent to an
// output until /clockwork/midi/in/enable or /clockwork/midi/out/enable says so.
// "*" opens or sends to every port.
//
// The Web MIDI access is INJECTABLE (`requestAccess`): a test hands in a fake
// with the same shape, and the whole manager runs under node against the
// real wasm core. The default asks the browser.

import { timetagToPerfMs, perfMsToTimetag } from "./timetag.js";
import init, {
  midi_in_osc_at,
  midi_in_fields,
  midi_out_decode,
  midi_ports_osc,
  midi_ports_reply_osc,
  normalize_name,
  WasmClockEstimator,
} from "../../dist/midi/clockwork_midi.js";

export class MidiManager {
  /**
   * @param {object} [options]
   * @param {() => Promise<MIDIAccess>} [options.requestAccess] how to get Web
   *   MIDI access; defaults to navigator.requestMIDIAccess({ sysex: true }).
   * @param {string|URL|Uint8Array} [options.wasm] where the core's wasm is,
   *   or its bytes; defaults to beside the glue module.
   * @param {(perfMs: number) => number} [options.now] performance.now, injectable.
   */
  constructor(options = {}) {
    this._requestAccess = options.requestAccess
      ?? (() => {
        if (typeof navigator === "undefined" || !navigator.requestMIDIAccess)
          throw new Error("Web MIDI API unavailable");
        return navigator.requestMIDIAccess({ sysex: true });
      });
    this._wasm = options.wasm;
    this._now = options.now ?? (() => performance.now());
    this._access = null;
    this._inputs = new Map(); // normalized name -> MIDIInput
    this._outputs = new Map(); // normalized name -> MIDIOutput
    this._inEnabled = new Set(); // normalized names heard from
    this._outEnabled = new Set(); // normalized names sent to
    this._clockMuted = new Set(); // inputs whose clock is ignored (clock/sync 0)
    this._estimators = new Map(); // normalized name -> WasmClockEstimator
    this._onEvent = null; // (Uint8Array osc) => void  — /clockwork/midi/in/* OSC packet
    this._onMessage = null; // (Array [kind, port, ...ints]) => void  — structured
    this._onPorts = null; // (Uint8Array osc) => void  — /clockwork/midi/ports push
    this._onTempo = null; // (port, bpm) => void  — clock-in
    this._lastPortsKey = null; // last emitted port list, to suppress no-op pushes
    this._onStateChange = () => this._refresh(true);
  }

  // Load the wasm core and acquire Web MIDI. Resolves once ports are enumerated.
  async init() {
    await init(this._wasm !== undefined ? { module_or_path: this._wasm } : undefined);
    this._access = await this._requestAccess();
    this._access.onstatechange = this._onStateChange;
    this._refresh(false);
    return this;
  }

  dispose() {
    if (this._access) this._access.onstatechange = null;
    for (const input of this._inputs.values()) input.onmidimessage = null;
    this._inputs.clear();
    this._outputs.clear();
    this._inEnabled.clear();
    this._outEnabled.clear();
    this._estimators.clear();
    this._access = null;
  }

  onEvent(cb) { this._onEvent = cb; }
  // Structured inbound events: cb receives a flat [kind, port, ...ints] array
  // (e.g. ["note_on", "kbd", 1, 60, 100]) with no OSC encode/decode round-trip.
  // Takes precedence over onEvent when both are set.
  onMessage(cb) { this._onMessage = cb; }
  // A /clockwork/midi/ports push, as bytes: on a hotplug or an enable change.
  onPorts(cb) { this._onPorts = cb; }
  onTempo(cb) { this._onTempo = cb; }

  // ── Ports ────────────────────────────────────────────────────────────────

  // [[name, enabled], ...] for inputs and outputs, in enumeration order.
  portLists() {
    const ins = [...this._inputs.keys()].map((n) => [n, this._inEnabled.has(n)]);
    const outs = [...this._outputs.keys()].map((n) => [n, this._outEnabled.has(n)]);
    return { ins, outs };
  }

  // The /clockwork/midi/ports.reply packet for the current state.
  portsReply() {
    const { ins, outs } = this.portLists();
    return midi_ports_reply_osc(
      ins.map((r) => r[0]), Uint8Array.from(ins, (r) => (r[1] ? 1 : 0)),
      outs.map((r) => r[0]), Uint8Array.from(outs, (r) => (r[1] ? 1 : 0)));
  }

  _portsPush() {
    const { ins, outs } = this.portLists();
    return midi_ports_osc(
      ins.map((r) => r[0]), Uint8Array.from(ins, (r) => (r[1] ? 1 : 0)),
      outs.map((r) => r[0]), Uint8Array.from(outs, (r) => (r[1] ? 1 : 0)));
  }

  // Open (or close) a port for input or output — "*" for every port. A name
  // nobody has is remembered: enabling a port before it is plugged in is a
  // normal thing to do, and it opens when it appears. Pushes /clockwork/midi/ports.
  enable(port, input, enabled) {
    const set = input ? this._inEnabled : this._outEnabled;
    const known = input ? this._inputs : this._outputs;
    const names = port === "*" ? [...known.keys()] : [normalize_name(port)];
    for (const name of names) {
      if (enabled) set.add(name); else set.delete(name);
    }
    this._push(true);
  }

  // Re-enumerate and push /clockwork/midi/ports whether or not anything changed —
  // a refresh is a request for the list.
  refresh() {
    this._refresh(false);
    this._push(true);
  }

  // Ignore (or heed) the MIDI clock arriving on an input: /clockwork/midi/clock/sync.
  clockSync(port, enabled) {
    const names = port === "*" ? [...this._inputs.keys()] : [normalize_name(port)];
    for (const name of names) {
      if (enabled) this._clockMuted.delete(name); else this._clockMuted.add(name);
      this._estimators.get(name)?.reset();
    }
  }

  // One immediate 0xF8 on a port ("*" = every enabled output): /clockwork/midi/clock/tick.
  tick(port) {
    this._sendRaw(port, new Uint8Array([0xf8]), undefined);
  }

  _refresh(fromStateChange) {
    for (const input of this._inputs.values()) input.onmidimessage = null;
    this._inputs.clear();
    this._outputs.clear();
    for (const input of this._access.inputs.values()) {
      const name = normalize_name(input.name || input.id);
      this._inputs.set(name, input);
      input.onmidimessage = (e) => this._onInput(name, e);
    }
    for (const output of this._access.outputs.values()) {
      const name = normalize_name(output.name || output.id);
      this._outputs.set(name, output);
    }
    // Web MIDI fires statechange for transient/duplicate transitions; only
    // push when the list actually changed, mirroring the native
    // clockwork_midi_refresh "broadcast only on change" behaviour.
    if (fromStateChange) this._push(false);
  }

  _push(force) {
    const key = JSON.stringify(this.portLists());
    if (!force && key === this._lastPortsKey) return;
    this._lastPortsKey = key;
    if (this._onPorts) this._onPorts(this._portsPush());
  }

  _onInput(port, event) {
    if (!this._inEnabled.has(port)) return;   // closed: not heard from
    const bytes = event.data;
    // Clock pulses feed the estimator (→ tempo), never surfaced as events.
    if (bytes.length === 1 && bytes[0] === 0xf8) {
      if (this._clockMuted.has(port)) return;
      let est = this._estimators.get(port);
      if (!est) {
        est = new WasmClockEstimator();
        this._estimators.set(port, est);
      }
      const bpm = est.update(event.timeStamp * 1000.0); // ms → µs
      if (bpm != null && this._onTempo) this._onTempo(port, bpm);
      return;
    }
    // Prefer the structured fast path; fall back to OSC bytes for consumers
    // (e.g. the native-shaped engine ingress) that want the wire form.
    if (this._onMessage) {
      const fields = midi_in_fields(port, bytes);
      if (fields) this._onMessage(fields);
      return;
    }
    // The wire form carries the moment the bytes arrived (the event's own
    // timestamp, in performance.now() terms) as a trailing timetag: a guest
    // placing the note on its own timeline wants when it was played, not when
    // it reached the audio thread. Both are the same shape natively.
    const when = perfMsToTimetag(event.timeStamp ?? this._now());
    const osc = midi_in_osc_at(port, bytes, when);
    if (osc && this._onEvent) this._onEvent(osc);
  }

  // ── Sends ────────────────────────────────────────────────────────────────

  // Send a /clockwork/midi/out/* OSC packet to hardware.
  //
  // The time comes from the verb itself — the packed form is
  // [when: 8 LE][portLen][port][bytes] — and is converted here from an OSC
  // timetag to the DOMHighResTimeStamp the Web MIDI API wants. `timestampMs`
  // remains an explicit override for a caller that has already decided —
  // the host front, which carries the time of a verb the scheduler fired.
  //
  // This is where a timestamp is worth the most: the browser schedules the
  // send itself, so handing it a future time is tighter than racing to deliver
  // on time across the worklet/main boundary.
  //
  // Returns false for a packet that is not a send verb.
  sendOut(oscBytes, timestampMs) {
    const packed = midi_out_decode(oscBytes);
    if (!packed) return false;
    const when = new DataView(packed.buffer, packed.byteOffset, 8).getBigUint64(0, true);
    const at = timestampMs !== undefined ? timestampMs : timetagToPerfMs(when);
    const portLen = packed[8];
    const port = new TextDecoder().decode(packed.subarray(9, 9 + portLen));
    const raw = packed.subarray(9 + portLen);
    this._sendRaw(port, raw, at);
    return true;
  }

  // A guest's send, from a sink the engine opened onto `port` (the host is
  // the sink's endpoint on the web: clockwork_event_sink.h). Opening a sink
  // onto a port opens the port, as it does natively, so an output the client
  // never enabled is enabled here on first use — and the ports push says so.
  // `timestampMs` is the send's time as the front converted it; undefined is
  // now. "*" is every output.
  sendFromSink(port, raw, timestampMs) {
    const names = port === "*" ? [...this._outputs.keys()] : [normalize_name(port)];
    let opened = false;
    for (const name of names) {
      if (!this._outEnabled.has(name) && (port === "*" || this._outputs.has(name))) {
        this._outEnabled.add(name);
        opened = true;
      }
    }
    if (opened) this._push(true);
    this._sendRaw(port, raw, timestampMs);
  }

  // Bytes to one enabled output, or every enabled output for "*". A port that
  // is not open is not sent to, as natively.
  _sendRaw(port, raw, at) {
    if (port === "*") {
      for (const [name, out] of this._outputs) {
        if (this._outEnabled.has(name)) out.send(raw, at);
      }
      return;
    }
    const name = normalize_name(port);
    if (!this._outEnabled.has(name)) return;
    const out = this._outputs.get(name);
    if (out) out.send(raw, at);
  }
}
