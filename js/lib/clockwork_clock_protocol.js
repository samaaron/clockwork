// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * ClockworkClock cross-thread protocol — field offsets in the ClockworkClockState
 * SAB region and PM-mode worklet message types. Imported by both
 * js/lib/clockwork_clock.js and js/workers/clockwork_audio_worklet.js so the
 * protocol cannot drift between them.
 *
 * Mirrors src/shared_memory.h ClockworkClockState. Doubles are stored as
 * raw IEEE 754 bit-patterns in 64-bit atomics (BigInt64Array + Atomics
 * on the JS side; std::atomic<uint64_t> on the C++ side).
 *
 * Layout (40 bytes). Every time here is NTP SECONDS (since 1900), as a
 * double — not the microseconds the /clockwork/clock wire carries, and not
 * the milliseconds NTPTiming's drift and clock offsets are kept in:
 *   [0-7]   bpm                (double as uint64 bit-pattern: beats per minute)
 *   [8-15]  beat_origin_ntp    (double as uint64 bit-pattern: NTP seconds at which beat 0 was)
 *   [16-23] is_playing_at_ntp  (double as uint64 bit-pattern: NTP seconds the transport last changed)
 *   [24-27] is_playing         (uint32: 0 or 1)
 *   [28-31] flags              (uint32: bit-packed SC_FLAG_*)
 *   [32-35] meter              (uint32: (num << 16) | den, see packMeter)
 *   [36-39] reserved           (alignment pad: the region is 8-byte)
 */

import { retempoOrigin } from './clock_math.js';

// BigInt64Array indices (8-byte stride).
export const SC_BPM_I64                = 0;
export const SC_BEAT_ORIGIN_NTP_I64    = 1;
export const SC_IS_PLAYING_AT_NTP_I64  = 2;

// Int32Array indices (4-byte stride) — byte offset / 4.
export const SC_IS_PLAYING_I32         = 6;   // byte 24
export const SC_FLAGS_I32              = 7;   // byte 28
export const SC_METER_I32              = 8;   // byte 32

// Flag bit positions — must match src/shared_memory.h.
export const SC_FLAG_LINK_ENABLED       = 1 << 0;
export const SC_FLAG_START_STOP_SYNC    = 1 << 1;
export const SC_FLAG_LINK_AUDIO_PUBLISH = 1 << 2;

// PM-mode worklet message types. The main thread posts these and the
// worklet's onmessage handler dispatches on them.
export const ClockworkClockMessageType = Object.freeze({
  SET_SESSION_BPM:              'setSessionBpm',       // {bpm, nowNtp}: re-anchored at nowNtp
  SET_SESSION_IS_PLAYING:       'setSessionIsPlaying',
  SET_SESSION_BEAT_ORIGIN_NTP:  'setSessionBeatOriginNtp',
  SET_SESSION_METER:            'setSessionMeter',      // {num, den}
});

// ── The write protocol, once ──────────────────────────────────────────────
// Twins of the ClockworkClockState mutators in src/shared_memory.h. `views` is
// {bigInt: BigInt64Array, int32: Int32Array} over the same 32 bytes. The
// value is stored first and its KEY field last (bpm keys the origin,
// is_playing keys its timestamp), so a reader that loads the key sees the
// value anchored for it — Atomics are sequentially consistent, so ordering
// alone carries the guarantee here.

/** Tempo below 1 is clamped: the grid divides by it. */
export function clampBpm(bpm) {
  return (Number.isFinite(bpm) && bpm >= 1) ? bpm : 1;
}

/**
 * A meter a bar can be built from: at least one beat over a power-of-two
 * note value up to a 32nd. Twin of clockwork_timeline_meter_valid (rust/clockwork-clock).
 */
export function isValidMeter(num, den) {
  return Number.isInteger(num) && num >= 1 && [1, 2, 4, 8, 16, 32].includes(den);
}

// The meter travels as ONE word — numerator high, denominator low — so a
// reader can never see the new numerator over the old denominator. Twin of
// ClockworkClockState::packMeter.
export function packMeter(num, den) {
  return ((num << 16) | (den & 0xFFFF)) | 0;
}

export function unpackMeter(packed) {
  return { num: (packed >>> 16) & 0xFFFF, den: packed & 0xFFFF };
}

/**
 * The meter, as {num, den}. No meter packs to 0 (a numerator is >= 1), so a
 * zero word is a region nothing has initialised, and reads as the default
 * 4/4 — the same answer readClockworkClock gives for a missing state.
 */
export function readClockMeter(views) {
  const packed = Atomics.load(views.int32, SC_METER_I32);
  return packed === 0 ? { num: 4, den: 4 } : unpackMeter(packed);
}

/** Set the meter. The grid is untouched: bars are counted from beat 0. */
export function writeClockMeter(views, num, den) {
  Atomics.store(views.int32, SC_METER_I32, packMeter(num, den));
}

export function readClockBpm(views) {
  return bitsToDouble(Atomics.load(views.bigInt, SC_BPM_I64));
}

export function readClockOrigin(views) {
  return bitsToDouble(Atomics.load(views.bigInt, SC_BEAT_ORIGIN_NTP_I64));
}

/** Publish a grid: origin first, then the tempo. */
export function writeClockTempo(views, bpm, originNtp) {
  Atomics.store(views.bigInt, SC_BEAT_ORIGIN_NTP_I64, doubleToBits(originNtp));
  Atomics.store(views.bigInt, SC_BPM_I64, doubleToBits(clampBpm(bpm)));
}

/**
 * Change the tempo without moving the beat playing at `nowNtp`
 * (clock_math.js retempoOrigin). Returns the new origin.
 */
export function retempoClock(views, bpm, nowNtp) {
  const newBpm = clampBpm(bpm);
  const origin = retempoOrigin(readClockOrigin(views), readClockBpm(views), newBpm, nowNtp);
  writeClockTempo(views, newBpm, origin);
  return origin;
}

/** Move the grid under the current tempo. */
export function writeClockOrigin(views, originNtp) {
  Atomics.store(views.bigInt, SC_BEAT_ORIGIN_NTP_I64, doubleToBits(originNtp));
}

/** Timestamp first, then the flag. */
export function writeClockTransport(views, playing, atNtp) {
  Atomics.store(views.bigInt, SC_IS_PLAYING_AT_NTP_I64, doubleToBits(atNtp));
  Atomics.store(views.int32, SC_IS_PLAYING_I32, playing ? 1 : 0);
}

// Reusable scratch buffer for double ↔ BigInt64 conversion. Single-thread
// JS access; no contention. Hoisted to module scope so setBpm / getBpm /
// etc. don't allocate per call.
const _scratchBuf = new ArrayBuffer(8);
const _scratchF64 = new Float64Array(_scratchBuf);
const _scratchI64 = new BigInt64Array(_scratchBuf);

export function doubleToBits(v) {
  _scratchF64[0] = v;
  return _scratchI64[0];
}

export function bitsToDouble(bits) {
  _scratchI64[0] = bits;
  return _scratchF64[0];
}
