// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * host_front.js — the web host's front: the far end of clockwork's chain.
 *
 * In the worklet the engine has no NRT thread, so a /clockwork/ verb the
 * audio thread does not answer itself (ping, echo, the clock snapshot,
 * sched/flush, the asset hand-off) is FORWARDED to the host: sent back out
 * over the egress to the client that sent it, wrapped in a bundle whose
 * timetag is the call's time — 1 for a verb that never waited, the scheduled
 * moment for one the scheduler fired (src/audio_processor.cpp,
 * clockwork_host_forward_route). This is where those land. It is the web
 * counterpart of the native control pass: the main thread is the only place
 * Web MIDI and the Gamepad API exist, so it answers /clockwork/midi/ and
 * /clockwork/gamepad/ here, through MidiManager and GamepadManager, and
 * REFUSES anything else under the prefix by name — exactly what the native
 * gateway does at the far end of its hop. Nothing under the prefix falls
 * through to silence.
 *
 * Every reply the front makes is delivered back into the client as an
 * inbound frame, through the same path an engine reply takes, so a client
 * cannot tell which side answered — the native /clockwork/midi/* contract
 * holds here to the byte, because the payloads are encoded by the same Rust
 * core.
 *
 * An EVENT — what the keyboard played, what the pad did, a ports or devices
 * change — is not delivered: it is sent INTO the engine, through ingress,
 * carrying the moment it arrived. Natively a subsystem's callback writes its
 * events into the same ring. From there one route on the audio thread
 * (clockwork_event_route) sends them out over the egress to the subsystem's
 * subscribers and hands them to the guest if it asked (DspInfo::wants_events)
 * — so a client or a guest hears an event the same way whichever side
 * produced it. On the web the one client hears every egress frame, so the
 * subscription rule is kept here: an event that comes back before the client
 * subscribed is dropped, as the native transport would not have sent it.
 */
import { clockworkSys, isClockworkSys } from "./clockwork_sys.js";
import * as oscFast from "./osc_fast.js";
import { timetagToPerfMs } from "./timetag.js";
import { MidiManager } from "./midi_manager.js";
import { GamepadManager } from "./gamepad_manager.js";

const MIDI_PREFIX = clockworkSys("midi/");
const GAMEPAD_PREFIX = clockworkSys("gamepad/");

// The reasons a refusal names. Strings a client can match on.
export const REASON_NO_MIDI =
  "MIDI is not enabled on this host: new Clockwork({ midi: true })";
export const REASON_NO_GAMEPAD =
  "gamepad is not enabled on this host: new Clockwork({ gamepad: true })";
export const REASON_NOT_ON_WEB = "not available on the web host";
export const REASON_UNKNOWN = "unknown clockwork verb";
export const REASON_MALFORMED = "malformed";

export class HostFront {
  /**
   * @param {object} options
   * @param {boolean|object} [options.midi=false] enable Web MIDI; an object
   *   is passed to MidiManager (requestAccess, wasm — for tests and
   *   non-standard hosts).
   * @param {boolean|object} [options.gamepad=false] enable the Gamepad API;
   *   an object is passed to GamepadManager.
   * @param {string} [options.wasmBaseURL] where clockwork_midi_bg.wasm and
   *   clockwork_gamepad_bg.wasm are, unless the manager options say.
   * @param {(bytes: Uint8Array) => void} options.deliver an inbound frame
   *   for the client, as if from the engine.
   * @param {(bytes: Uint8Array) => void} options.ingest an event for the
   *   engine, into ingress, as a subsystem's callback would write it.
   */
  constructor({ midi = false, gamepad = false, wasmBaseURL = null, deliver, ingest } = {}) {
    this._midiOptions = midi;
    this._gamepadOptions = gamepad;
    this._wasmBaseURL = wasmBaseURL;
    this._deliver = deliver;
    this._ingest = ingest ?? deliver;
    this._midi = null;
    this._gamepad = null;
    this._midiSubscribed = false;
    this._gamepadSubscribed = false;
  }

  get midi() { return this._midi; }
  get gamepad() { return this._gamepad; }

  async init() {
    if (this._midiOptions) {
      const opts = typeof this._midiOptions === "object" ? { ...this._midiOptions } : {};
      if (opts.wasm === undefined && this._wasmBaseURL)
        opts.wasm = this._wasmBaseURL + "clockwork_midi_bg.wasm";
      this._midi = new MidiManager(opts);
      this._midi.onEvent((osc) => this._ingest(osc));
      this._midi.onPorts((osc) => this._ingest(osc));
      this._midi.onTempo((port, bpm) => {
        this._ingest(oscFast.copyEncoded(
          oscFast.encodeMessage(clockworkSys("midi/in/clock_bpm"), [port, { type: "float", value: bpm }])));
      });
      await this._midi.init();
    }
    if (this._gamepadOptions) {
      const opts = typeof this._gamepadOptions === "object" ? { ...this._gamepadOptions } : {};
      if (opts.wasm === undefined && this._wasmBaseURL)
        opts.wasm = this._wasmBaseURL + "clockwork_gamepad_bg.wasm";
      this._gamepad = new GamepadManager(opts);
      this._gamepad.onEvent((osc) => this._ingest(osc));
      this._gamepad.onDevices((osc) => this._ingest(osc));
      await this._gamepad.init();
    }
    return this;
  }

  dispose() {
    this._midi?.dispose();
    this._gamepad?.dispose();
    this._midi = null;
    this._gamepad = null;
    this._midiSubscribed = false;
    this._gamepadSubscribed = false;
  }

  /**
   * One frame off the egress. True iff it was a forwarded verb and the front
   * took it — answered, acted on, or refused. Everything else (a reply, a
   * push, the guest's traffic) is left for the client: false.
   *
   * A forwarded verb is a bundle with one element under the prefix. Nothing
   * else on the egress has that shape.
   */
  take(oscData) {
    if (!oscFast.isBundle(oscData)) return this._takeEvent(oscData);
    if (oscData.length < 20) return false;
    const view = new DataView(oscData.buffer, oscData.byteOffset, oscData.byteLength);
    const size = view.getUint32(16, false);
    if (20 + size > oscData.length) return false;
    const element = oscData.subarray(20, 20 + size);
    if (element[0] !== 0x2f) return false;   // '/': a message, not a nested bundle
    let msg;
    try { msg = oscFast.decodeMessage(element); } catch { return false; }
    const address = msg[0];
    if (!isClockworkSys(address)) return false;
    const when = view.getBigUint64(8, false);
    const whenMs = timetagToPerfMs(when);   // undefined: immediately
    this._handle(address, msg.slice(1), element, whenMs);
    return true;
  }

  // An event coming back off the egress (clockwork_event_route): the
  // client's to hear if it subscribed to that subsystem, dropped if not.
  // Cheap: only the address is read, and only under the two prefixes.
  _takeEvent(oscData) {
    if (oscData[0] !== 0x2f) return false;
    let end = 0;
    while (end < oscData.length && oscData[end] !== 0) end++;
    if (end > 48) return false;   // longer than any event address
    const address = String.fromCharCode.apply(null, oscData.subarray(0, end));
    const isMidi = address.startsWith(MIDI_PREFIX + "in/") || address === clockworkSys("midi/ports");
    const isPad = address.startsWith(GAMEPAD_PREFIX + "in/") || address === clockworkSys("gamepad/devices");
    if (isMidi) return !this._midiSubscribed;
    if (isPad) return !this._gamepadSubscribed;
    return false;
  }

  _handle(address, args, element, whenMs) {
    if (address.startsWith(MIDI_PREFIX)) return this._midiVerb(address, args, element, whenMs);
    if (address.startsWith(GAMEPAD_PREFIX)) return this._gamepadVerb(address, args, element);
    this._refuse(address, REASON_UNKNOWN);
  }

  _refuse(address, reason) {
    this._deliver(oscFast.copyEncoded(oscFast.encodeMessage(clockworkSys("error"), [address, reason])));
  }

  // The subscribe verbs ack with the trailing int32 the request carried, and
  // only then (SubscribeAck.h): a token-less subscribe stays silent.
  _ack(replyAddress, args) {
    const last = args[args.length - 1];
    if (typeof last === "number" && Number.isInteger(last))
      this._deliver(oscFast.copyEncoded(oscFast.encodeMessage(replyAddress, [{ type: "int", value: last }])));
  }

  _midiVerb(address, args, element, whenMs) {
    const m = this._midi;
    if (!m) return this._refuse(address, REASON_NO_MIDI);
    const verb = address.slice(MIDI_PREFIX.length);
    const port = typeof args[0] === "string" ? args[0] : "*";
    const flag = (i) => typeof args[i] === "number" && args[i] !== 0;
    switch (verb) {
      case "ports/list":
      case "ports/get":
        return this._deliver(m.portsReply());
      case "in/enable":
        return m.enable(port, true, flag(1));
      case "out/enable":
        return m.enable(port, false, flag(1));
      case "refresh":
        return m.refresh();
      case "notify/subscribe":
        this._midiSubscribed = true;
        this._deliver(m.portsReply());   // the snapshot a new subscriber gets
        return this._ack(clockworkSys("midi/notify/subscribe.reply"), args);
      case "notify/unsubscribe":
        this._midiSubscribed = false;
        return;
      case "sink/send": {
        // A GUEST'S send: the engine's sink endpoint on the web is this
        // host (clockwork_event_sink.h, clockwork_sink_set_host_emit). The
        // bytes go to the port with the send's time, and the port is opened
        // on demand — opening a sink onto a port opens the port, as natively.
        const raw = args[1];
        if (!(raw instanceof Uint8Array) || raw.length === 0)
          return this._refuse(address, REASON_MALFORMED);
        return m.sendFromSink(port, raw, whenMs);
      }
      case "clock/tick":
        return m.tick(port);
      case "clock/sync":
        return m.clockSync(port, flag(1));
      case "clock/beat":
      case "clock/follow":
      case "clock/unfollow":
      case "clock/followers":
        // MidiClockOut is the engine's, and the worklet has none to generate
        // from: a beat spread by ClockworkClock needs the audio thread's clock.
        return this._refuse(address, REASON_NOT_ON_WEB);
      default:
        if (verb.startsWith("out/")) {
          if (!m.sendOut(element, whenMs)) this._refuse(address, REASON_MALFORMED);
          return;
        }
        return this._refuse(address, REASON_UNKNOWN);
    }
  }

  _gamepadVerb(address, args, element) {
    const g = this._gamepad;
    if (!g) return this._refuse(address, REASON_NO_GAMEPAD);
    const verb = address.slice(GAMEPAD_PREFIX.length);
    const pad = typeof args[0] === "string" ? args[0] : "*";
    switch (verb) {
      case "devices/list":
      case "devices/get":
        return this._deliver(g.devicesReply());
      case "enable":
        return g.enable(pad, typeof args[1] === "number" && args[1] !== 0);
      case "refresh":
        return g.refresh();
      case "notify/subscribe":
        this._gamepadSubscribed = true;
        this._deliver(g.devicesReply());
        return this._ack(clockworkSys("gamepad/notify/subscribe.reply"), args);
      case "notify/unsubscribe":
        this._gamepadSubscribed = false;
        return;
      default:
        if (verb.startsWith("out/")) {
          if (!g.sendOut(element)) this._refuse(address, REASON_MALFORMED);
          return;
        }
        return this._refuse(address, REASON_UNKNOWN);
    }
  }
}
