// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * ClockworkClock — single JS-side authority for session state and time.
 * Mirrors the C++ ClockworkClock API (src/clock/ClockworkClock.h).
 *
 * Owns:
 *   - Session state: BPM, transport, beat origin, meter. SAB-mode writes the
 *     ClockworkClockState region directly via BigInt64 atomics; PM-mode
 *     posts to the worklet which writes the same region.
 *   - Time: ntp_start_time / drift / clock-offset via a composed
 *     private NTPTiming helper.
 */

import { NTPTiming, NTP_EPOCH_OFFSET } from './ntp_timing.js';
import {
  SC_BPM_I64,
  SC_BEAT_ORIGIN_NTP_I64,
  SC_IS_PLAYING_AT_NTP_I64,
  SC_IS_PLAYING_I32,
  ClockworkClockMessageType,
  bitsToDouble, clampBpm, isValidMeter,
  retempoClock, writeClockOrigin, writeClockTransport,
  readClockMeter, writeClockMeter,
} from './clockwork_clock_protocol.js';
import { beatAt, timeAtBeat, originFor, retempoOrigin, wrapPhase } from './clock_math.js';

export class ClockworkClock {
  #mode;
  #workletPort;
  #audioContext;

  // SAB views over the ClockworkClockState region (same bytes, two strides).
  #sabBigInt;  // BigInt64Array — for the three double fields
  #sabInt32;   // Int32Array    — for the is_playing field
  #sabViews;   // {bigInt, int32} — what the clockwork_clock_protocol writers take

  // PM-mode local copies. In SAB mode these are also kept up-to-date so
  // getters have a fallback before initSharedViews has run.
  #localBpm = 120.0;
  #localBeatOriginNtp = 0.0;
  #localIsPlaying = false;
  #localIsPlayingAtNtp = 0.0;
  #localMeter = { num: 4, den: 4 };

  #ntp;

  /**
   * @param {Object} options
   * @param {'sab'|'postMessage'} options.mode
   * @param {AudioContext} options.audioContext
   * @param {MessagePort} [options.workletPort] — required in PM mode
   */
  constructor(options = {}) {
    this.#mode = options.mode || 'sab';
    this.#workletPort = options.workletPort || null;
    this.#audioContext = options.audioContext || null;
    this.#ntp = new NTPTiming({
      mode: this.#mode,
      audioContext: options.audioContext,
      workletPort: this.#workletPort,
    });
  }

  /**
   * Initialize SAB views for session state and time.
   * @param {SharedArrayBuffer} sharedBuffer
   * @param {number} ringBufferBase
   * @param {Object} bufferConstants — must include CLOCK_STATE_START / _SIZE
   */
  initSharedViews(sharedBuffer, ringBufferBase, bufferConstants) {
    if (this.#mode === 'sab') {
      const base = ringBufferBase + bufferConstants.CLOCK_STATE_START;
      const size = bufferConstants.CLOCK_STATE_SIZE;
      this.#sabBigInt = new BigInt64Array(sharedBuffer, base, size / 8);
      this.#sabInt32  = new Int32Array(sharedBuffer, base, size / 4);
      this.#sabViews  = { bigInt: this.#sabBigInt, int32: this.#sabInt32 };
    }
    this.#ntp.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);
  }

  /** @param {MessagePort} port */
  setWorkletPort(port) { this.#workletPort = port; }

  updateAudioContext(audioContext) {
    this.#audioContext = audioContext;
    this.#ntp.updateAudioContext(audioContext);
  }

  // ── Time / drift surface (delegates to private NTPTiming) ──────────────

  async initialize() { await this.#ntp.initialize(); }
  resync()           { this.#ntp.resync(); }
  startDriftTimer()  { this.#ntp.startDriftTimer(); }
  stopDriftTimer()   { this.#ntp.stopDriftTimer(); }
  updateDriftOffset(){ this.#ntp.updateDriftOffset(); }
  getDriftOffset()   { return this.#ntp.getDriftOffset(); }
  getNTPStartTime()  { return this.#ntp.getNTPStartTime(); }
  getClockOffset()   { return this.#ntp.getClockOffset(); }
  setClockOffset(s)  { this.#ntp.setClockOffset(s); }
  reset()            { this.#ntp.reset(); }

  // ── Session mutators ───────────────────────────────────────────────────

  /**
   * Change the tempo without moving the beat that is playing: the grid is
   * re-anchored at `nowNtp` (clock_math.js retempoOrigin), which defaults to
   * the audio thread's now. Before this, setBpm stored the tempo against the
   * old origin and every web tempo change jumped the beat — the same defect
   * the C++ sessions had (test_clock_tempo_change.cpp), on the path a page
   * actually calls.
   *
   * @param {number} bpm
   * @param {number} [atNtpSeconds=0] — honoured by a Link backing
   * @param {number} [nowNtp] — the instant whose beat is held; tests pass one
   */
  setBpm(bpm, atNtpSeconds = 0, nowNtp = this.#nowForRetempo()) {
    bpm = clampBpm(bpm);
    this.#localBeatOriginNtp = retempoOrigin(this.#localBeatOriginNtp, this.#localBpm, bpm, nowNtp);
    this.#localBpm = bpm;
    if (this.#sabViews) {
      retempoClock(this.#sabViews, bpm, nowNtp);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: ClockworkClockMessageType.SET_SESSION_BPM,
        bpm, atNtpSeconds, nowNtp,
      });
    }
  }

  // Audio-thread NTP when there is an AudioContext to read it from, else the
  // wall clock: either way it is "now" in the domain beatAtTime is asked in.
  #nowForRetempo() {
    return this.#audioContext ? this.now() : this.wallNow();
  }

  /**
   * @param {boolean} playing
   * @param {number} [atNtpSeconds=0]
   */
  setIsPlaying(playing, atNtpSeconds = 0) {
    this.#localIsPlaying = !!playing;
    this.#localIsPlayingAtNtp = atNtpSeconds;
    if (this.#sabViews) {
      writeClockTransport(this.#sabViews, playing, atNtpSeconds);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: ClockworkClockMessageType.SET_SESSION_IS_PLAYING,
        isPlaying: this.#localIsPlaying, atNtpSeconds,
      });
    }
  }

  /**
   * The meter: how quarter-note beats group into bars. 4/4 until set. A
   * meter isValidMeter refuses is ignored and false is returned; the grid
   * is never touched, because bars are counted from beat 0 whatever the
   * meter. Twin of ClockworkClock::setMeter (C++).
   *
   * @param {number} num
   * @param {number} den — 1, 2, 4, 8, 16 or 32
   * @returns {boolean}
   */
  setMeter(num, den) {
    if (!isValidMeter(num, den)) return false;
    this.#localMeter = { num, den };
    if (this.#sabViews) {
      writeClockMeter(this.#sabViews, num, den);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: ClockworkClockMessageType.SET_SESSION_METER, num, den,
      });
    }
    return true;
  }

  /**
   * Link integration (peer discovery, tempo sync, audio sharing) is
   * native-only — the browser AudioWorklet can't host Link's network
   * thread or do UDP multicast. On native, set via OSC:
   * /clockwork/clock/visibility (int 0|1|2). On web this is a no-op.
   *
   * @param {boolean} enabled
   */
  setLinkEnabled(enabled) {
    if (enabled) {
      // eslint-disable-next-line no-console
      console.warn(
        '[ClockworkClock] setLinkEnabled(true) ignored on web — Link is ' +
        'native-only. Use the native build + /clockwork/clock/visibility.');
    }
  }

  /**
   * @param {number} beat
   * @param {number} atNtpSeconds
   * @param {number} quantum
   */
  requestBeatAtTime(beat, atNtpSeconds, quantum) {
    const newOrigin = originFor(beat, atNtpSeconds, this.getBpm());
    this.#localBeatOriginNtp = newOrigin;
    if (this.#sabViews) {
      writeClockOrigin(this.#sabViews, newOrigin);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: ClockworkClockMessageType.SET_SESSION_BEAT_ORIGIN_NTP,
        beatOriginNtp: newOrigin,
      });
    }
  }

  forceBeatAtTime(beat, atNtpSeconds, quantum) {
    this.requestBeatAtTime(beat, atNtpSeconds, quantum);
  }

  // ── Session getters ────────────────────────────────────────────────────

  getBpm() {
    if (this.#sabBigInt) return bitsToDouble(Atomics.load(this.#sabBigInt, SC_BPM_I64));
    return this.#localBpm;
  }

  isPlaying() {
    if (this.#sabInt32) return Atomics.load(this.#sabInt32, SC_IS_PLAYING_I32) !== 0;
    return this.#localIsPlaying;
  }

  getBeatOriginNtp() {
    if (this.#sabBigInt) return bitsToDouble(Atomics.load(this.#sabBigInt, SC_BEAT_ORIGIN_NTP_I64));
    return this.#localBeatOriginNtp;
  }

  getIsPlayingAtNtp() {
    if (this.#sabBigInt) return bitsToDouble(Atomics.load(this.#sabBigInt, SC_IS_PLAYING_AT_NTP_I64));
    return this.#localIsPlayingAtNtp;
  }

  /** @returns {{num: number, den: number}} */
  getMeter() {
    if (this.#sabViews) return readClockMeter(this.#sabViews);
    return { ...this.#localMeter };
  }

  isLinkEnabled() { return false; }
  numPeers()      { return 0; }

  /**
   * Current NTP time as seen by the audio thread — `AudioContext.currentTime`
   * + the engine's NTP-start anchor + drift + global offset.
   *
   * Use this to schedule events relative to "now in audio time":
   *
   *   sonic.sendOSC(osc.encodeBundle(sonic.clock.now() + 0.05, packets));
   *
   * That bundle fires 50 ms of *audio time* from now. The audio thread is
   * reading the same clock, so the two sides always agree — no skew if
   * AudioContext is throttled, no need to know the NTP-start formula.
   */
  now() {
    if (!this.#audioContext) return 0;
    return this.nowAt(this.#audioContext.currentTime);
  }

  /**
   * Compute audio-thread NTP for a specific `AudioContext.currentTime`.
   * `now()` calls this with the live `currentTime`; advanced callers can
   * pass a specific value (e.g. from `audioContext.getOutputTimestamp()`
   * for tight sample-aligned scheduling).
   */
  nowAt(audioCurrentTime) {
    // getDriftOffset / getClockOffset both return milliseconds; divide
    // by 1000 to get seconds. (C++ reads raw microseconds from the SAB
    // and divides by 1e6, getting the same seconds value with µs
    // precision — JS-side precision is ms because NTPTiming rounds.)
    return audioCurrentTime
         + this.getNTPStartTime()
         + this.getDriftOffset() / 1000
         + this.getClockOffset() / 1000;
  }

  /**
   * Current NTP time as seen by the system wall clock — `performance.now()`
   * + `performance.timeOrigin`, converted to NTP. Independent of the audio
   * clock; useful when matching against external wall-clock events (e.g.
   * network MIDI, NTP sources). For scheduling engine events, prefer
   * {@link now} so the audio thread and the scheduler agree.
   */
  wallNow() {
    return (performance.timeOrigin + performance.now()) / 1000 + NTP_EPOCH_OFFSET;
  }

  // ── Beat math ──────────────────────────────────────────────────────────
  // Reads bpm and beat_origin individually — no multi-field coherence
  // guarantee. Today's only callers are app-thread (single-writer JS).

  beatAtTime(ntpSeconds, quantum) {
    return beatAt(ntpSeconds, this.getBeatOriginNtp(), this.getBpm());
  }

  phaseAtTime(ntpSeconds, quantum) {
    return wrapPhase(this.beatAtTime(ntpSeconds, quantum), quantum);
  }

  timeAtBeat(beat, quantum) {
    return timeAtBeat(beat, this.getBeatOriginNtp(), this.getBpm());
  }
}
