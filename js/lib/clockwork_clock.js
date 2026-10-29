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
 *   - Time: read from the engine, which keeps its own clock on every target
 *     (clock/TimeSource.h) and publishes it in the arena's sample clock
 *     (setNowSource); and the host's global clock offset, written to the
 *     arena's anchor word for the engine to add.
 */

import { NTP_EPOCH_OFFSET } from './timetag.js';
import {
  SC_BPM_I64,
  SC_BEAT_ORIGIN_NTP_I64,
  SC_IS_PLAYING_AT_NTP_I64,
  SC_IS_PLAYING_I32,
  ClockworkClockMessageType,
  bitsToDouble, clampBpm, isValidMeter,
  retempoClock, writeClockOrigin, writeClockTransport,
  readClockMeter, writeClockMeter, readClockGeneration,
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
  #localGeneration = 0;

  #nowSource = () => 0;        // the engine's time, as a channel reads it (setNowSource)
  #globalOffsetView = null;    // SAB mode: the host's global offset word in the arena (ms)
  #localClockOffsetMs = 0;

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
  }

  /**
   * Initialize SAB views for session state and time.
   * @param {SharedArrayBuffer} sharedBuffer
   * @param {number} arenaBase
   * @param {Object} bufferConstants — must include CLOCK_STATE_START / _SIZE
   */
  initSharedViews(sharedBuffer, arenaBase, bufferConstants) {
    if (this.#mode === 'sab') {
      const base = arenaBase + bufferConstants.CLOCK_STATE_START;
      const size = bufferConstants.CLOCK_STATE_SIZE;
      this.#sabBigInt = new BigInt64Array(sharedBuffer, base, size / 8);
      this.#sabInt32  = new Int32Array(sharedBuffer, base, size / 4);
      this.#sabViews  = { bigInt: this.#sabBigInt, int32: this.#sabInt32 };
    }
    if (this.#mode === 'sab' && bufferConstants.GLOBAL_OFFSET_START !== undefined)
      this.#globalOffsetView = new Int32Array(sharedBuffer, arenaBase + bufferConstants.GLOBAL_OFFSET_START, 1);
  }

  /** @param {MessagePort} port */
  setWorkletPort(port) { this.#workletPort = port; }

  updateAudioContext(audioContext) {
    this.#audioContext = audioContext;
  }

  // ── Time ───────────────────────────────────────────────────────────────

  /** Where now() reads the engine's time: a channel's now() (OscChannel), which reads what the audio thread published. */
  setNowSource(fn) { this.#nowSource = fn; }
  /** The host's global clock offset (multi-system sync), in milliseconds, as last set here. */
  getClockOffset() { return this.#localClockOffsetMs; }
  /**
   * Shift the engine's clock by `offsetS` seconds, on every target (the arena's anchor word, which the engine's
   * clock adds to every answer): a deliberate shift for multi-system sync.
   */
  setClockOffset(offsetS) {
    const ms = Math.round(offsetS * 1000);
    this.#localClockOffsetMs = ms;
    if (this.#globalOffsetView) Atomics.store(this.#globalOffsetView, 0, ms);
    else if (this.#workletPort) this.#workletPort.postMessage({ type: 'setClockOffset', clockOffsetMs: ms });
  }

  // ── Session mutators ───────────────────────────────────────────────────

  /**
   * Change the tempo without moving the beat playing at the instant it
   * changes: the grid is re-anchored there (clock_math.js retempoOrigin).
   * That instant is `atNtpSeconds` when one is given — a scheduler changes
   * the tempo where the change will be heard, its schedule-ahead after now,
   * and the grid must agree with the beats it has already worked out — and
   * otherwise now. Storing the tempo against the old origin would jump the
   * beat on every tempo change (test_clock_tempo_change.cpp).
   *
   * @param {number} bpm
   * @param {number} [atNtpSeconds=0] — the instant the tempo changes; 0 is now
   * @param {number} [nowNtp] — now, when no instant is given; tests pass one
   */
  setBpm(bpm, atNtpSeconds = 0, nowNtp = this.#nowForRetempo()) {
    bpm = clampBpm(bpm);
    const pivot = atNtpSeconds > 0 ? atNtpSeconds : nowNtp;
    this.#localBeatOriginNtp = retempoOrigin(this.#localBeatOriginNtp, this.#localBpm, bpm, pivot);
    this.#localBpm = bpm;
    this.#localGeneration++;
    if (this.#sabViews) {
      retempoClock(this.#sabViews, bpm, pivot);
    } else if (this.#workletPort) {
      this.#workletPort.postMessage({
        type: ClockworkClockMessageType.SET_SESSION_BPM,
        bpm, atNtpSeconds, nowNtp: pivot,
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
    this.#localGeneration++;
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

  /**
   * One more each time the grid moves (a tempo change, a new origin), from
   * whichever writer moved it: this page, the worklet, a peer. A follower
   * with its own copy of the grid reads this before the grid and reads the
   * grid again when it has changed. In postMessage mode, only this clock's
   * own changes count.
   */
  getGeneration() {
    if (this.#sabViews) return readClockGeneration(this.#sabViews);
    return this.#localGeneration;
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
   * The engine's clock, in NTP seconds: the time its audio thread has reached, as it publishes it once a block
   * (the arena's sample clock; the same reading an OscChannel's now() takes on any thread). It stands still while
   * the audio does, and after a reload it is the engine's from its first block. 0 until the engine has rendered one.
   *
   * Use this to schedule events relative to "now in audio time":
   *
   *   sonic.sendOSC(osc.encodeBundle(sonic.clock.now() + 0.05, packets));
   *
   * That bundle fires 50 ms of *audio time* from now. The audio thread is reading the same clock, so the two sides
   * always agree — no skew if the AudioContext is throttled.
   */
  now() {
    return this.#nowSource();
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
