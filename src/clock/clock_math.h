// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clock_math.h — the shared clock arithmetic, once.
 *
 * The tempo-grid formula (beat = (t − origin) · bpm / 60), the NTP epoch
 * constant, and the NTP → OSC-timetag packing all appear in several unrelated
 * TUs (ClockworkClock NTP math, the session-of-one mirror, timeline free-run,
 * metrics, the standalone host, the scheduler ingest). One definition here so
 * they can't drift apart.
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

namespace clockwork {

// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
inline constexpr double kNtpEpochOffset = 2208988800.0;

// One second in NTP units. An NTP timestamp is 64-bit fixed point — 32 bits of
// seconds over 32 bits of fraction (RFC 5905 sec. 6) — so a second is 2^32.
inline constexpr double kNtpUnitsPerSecond = 4294967296.0;

// Link's own beat unit (ableton/link/Beats.hpp): beats travel as int64
// microbeats, as times travel as int64 micros.
inline constexpr double kMicrobeatsPerBeat = 1.0e6;

// Gain of the slow drift-correction IIRs (audio-thread NTP in TimeSource,
// Link-domain block stamp in BlockStamp): converge toward
// the reference at ~1% per audio callback — fast enough to track real drift,
// slow enough to damp callback-wake jitter. The block stamp also bounds each
// step, since audio placed by it hears every one.
inline constexpr double kDriftIirGain = 0.01;

// The most the audio thread's clock moves against its sample counter in one
// block, either way (TimeSource): everything is scheduled against this clock,
// and moving it moves what is already scheduled — with the bound, a bundle
// placed 50 ms ahead lands within 40 µs of where its sender worked it out.
// Two microseconds a block is 1500 ppm at 128 frames and 48 kHz: more than
// any two crystals disagree by, so real drift is followed in full, and the
// residual an anchor leaves (one wall reading can be off by a render burst,
// ~7 ms) is taken up over seconds rather than at once. Far under a block, so
// a reading never goes back.
inline constexpr double kClockSlewPerBlock = 2.0e-6;

// An anchor is one wall reading, and one reading can be out by a render burst
// on a host that reads Date.now() (~7 ms): more than the bound takes up in
// seconds. So an anchor settles: the clock holds its first reading for this
// many blocks (~170 ms at 128 frames and 48 kHz), then moves once to the
// median of the readings meanwhile — within a millisecond of the truth, and
// deaf to a callback that woke late — and the bound holds it from there. On a
// host whose readings are exact the median is the reading, and nothing moves.
inline constexpr uint32_t kAnchorSettleBlocks = 64;
// A settle that would move the clock BACK (the anchor read late: the clock
// ahead of the wall by a render burst, as a context filling its output buffer
// at start renders one) is taken up by slowing instead — this share of each
// block's length a block — so a reader never sees time go back and the clock
// is on the wall again within a few dozen blocks; what is scheduled meanwhile
// lands late by at most what is still to take up.
inline constexpr double kAnchorTakeUpShare = 0.125;

// The audio was away (clockwork_audio_back): the gap is the time that passed
// since the last block less the audio rendered in it, measured by the host as
// it hands each block over. Over this the host anchors the clock afresh at the
// block back, rather than letting it converge (TimeSource): a clock that
// converges from behind runs fast until it has caught up, and everything
// scheduled against it plays compressed. Under this the IIR takes it up, a
// bounded share a block (kClockSlewPerBlock), as it takes up a browser
// rendering its quanta in bursts (a reading swings by a device buffer, ~20 ms).
inline constexpr double kClockStepSeconds = 0.1;

inline double beatAt(double t, double origin, double bpm) {
    return (t - origin) * bpm / 60.0;
}

inline double timeAtBeat(double beat, double origin, double bpm) {
    return origin + beat * 60.0 / bpm;
}

// The origin that puts `beat` at time `t`.
inline double originFor(double beat, double t, double bpm) {
    return t - beat * 60.0 / bpm;
}

// A TEMPO CHANGE MUST NOT MOVE THE BEAT THAT IS PLAYING.
//
// beatAt(t) is (t − origin) · bpm / 60, so a new tempo against the old
// origin moves the current beat by the ratio of the two: forward when the
// tempo rises (everything scheduled is suddenly in the past), BACKWARDS when
// it falls (a client waiting on the next beat waits for the clock to climb
// back to a beat it had almost reached; `sleep 1` in a loop stops returning).
// This is the origin that holds the beat playing at `now` under `oldBpm`
// still at `now` under `newBpm`. An unanchored grid (origin 0 — nobody has
// placed it yet) or a nonsensical old tempo is returned untouched.
// Twin: js/lib/clock_math.js retempoOrigin.
inline double retempoOrigin(double origin, double oldBpm, double newBpm, double now) {
    if (origin == 0.0 || !(oldBpm >= 1.0)) return origin;
    return originFor(beatAt(now, origin, oldBpm), now, newBpm);
}

}  // namespace clockwork

// Current wall-clock time as NTP seconds (global name: referenced unqualified
// across the native tree and the shm reader side). The engine's TimeSource
// and every cross-process sample-clock reader must use this one formula.
inline double wallClockNTP() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch()).count()
         + clockwork::kNtpEpochOffset;
}

namespace clockwork {
// The host's monotonic clock, seconds from an origin of its own: what tells a
// gap in the audio from a system clock set by hand (ClockworkBlockTime).
inline double monotonicSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace clockwork

namespace clockwork {

// Non-negative phase of `beat` within `quantum` (0 when quantum <= 0).
inline double wrapPhase(double beat, double quantum) {
    if (quantum <= 0.0) return 0.0;
    double p = std::fmod(beat, quantum);
    if (p < 0.0) p += quantum;
    return p;
}

// NTP seconds (since 1900) → OSC 64-bit 32.32 fixed-point timetag.
inline int64_t ntpToOscTimetag(double ntpSeconds) {
    const uint32_t s = static_cast<uint32_t>(ntpSeconds);
    const uint32_t f = static_cast<uint32_t>((ntpSeconds - s) * kNtpUnitsPerSecond);
    return static_cast<int64_t>((static_cast<uint64_t>(s) << 32u) | f);
}

// The inverse: an OSC timetag → NTP seconds. Not a round trip to the bit —
// the fraction is 32 bits, a double's mantissa 53 — but exact for the
// block times the hosts stamp, which come from this pair.
inline double oscTimetagToNtp(int64_t timetag) {
    const uint64_t u = static_cast<uint64_t>(timetag);
    return static_cast<double>(u >> 32u)
         + static_cast<double>(u & 0xFFFFFFFFu) / kNtpUnitsPerSecond;
}

}  // namespace clockwork
