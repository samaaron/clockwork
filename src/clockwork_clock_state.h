// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_clock_state.h — the session clock as a DSP reads it.
 *
 * DspConfig::clock (dsp_api.h) points at a ClockworkClockState, and this is
 * the one clockwork header a guest includes to read it: the state's layout,
 * the snapshot, and readClockworkClock(), which applies the ordering rules so
 * a DSP author never has to. It is split out of shared_memory.h — the whole
 * arena layout — so that reading the clock does not mean knowing where
 * clockwork keeps anything else.
 */
#ifndef CLOCKWORK_CLOCK_STATE_H
#define CLOCKWORK_CLOCK_STATE_H

#include "clock/clock_math.h"   // beatAt, bitsToDouble, kDefaultBpm

#include <atomic>
#include <cstdint>
#include <cstring>

namespace clockwork {

// Double ↔ uint64 bit-pattern conversion. Centralised so the C++ mirror of
// ClockworkClockState (and any future SAB struct storing a double in a 64-bit
// atomic) has one bit-cast spelling.
//
// __builtin_bit_cast, not memcpy: ESP-IDF compiles every translation unit with
// -fno-builtin-memcpy, so an eight-byte memcpy here does not fold into a pair
// of loads — it emits a call to the library memcpy, on the audio thread, once a
// block through TimeSource::nowAt and again in every clock publish. The builtin
// has no such escape hatch, and it is the same
// reinterpretation memcpy performed, spelled so the compiler must do it in
// line. The memcpy form stays behind it so a compiler without the builtin
// (which includes emcc's C++17 default, where std::bit_cast is not available
// either) still builds.
#if defined(__has_builtin)
#  if __has_builtin(__builtin_bit_cast)
#    define CLOCKWORK_BIT_CAST_BUILTIN 1
#  endif
#endif

inline uint64_t doubleToBits(double v) {
#ifdef CLOCKWORK_BIT_CAST_BUILTIN
    return __builtin_bit_cast(uint64_t, v);
#else
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(double));
    return bits;
#endif
}

inline double bitsToDouble(uint64_t bits) {
#ifdef CLOCKWORK_BIT_CAST_BUILTIN
    return __builtin_bit_cast(double, bits);
#else
    double v;
    std::memcpy(&v, &bits, sizeof(double));
    return v;
#endif
}

// Session tempo a fresh boot opens at, before any peer, local set, or embedder
// override. Matches Ableton Link's own default; single source for the SAB seed,
// the Link session ctor, and Config::defaultBpm. An embedder
// overrides it at construction via Config::defaultBpm.
constexpr double kDefaultBpm = 120.0;

}  // namespace clockwork

// ClockworkClock session state. Has its own SAB region because it's engine
// state (the JS↔worklet transport on WASM depends on it), not observability
// — separating it from PerformanceMetrics keeps that struct honest.
//
// Bound on EVERY build: WASM at clockwork_clock_wasm_init, native at engine init
// (ClockworkEngine.cpp -> bindStateToShm).
//
// Each field is a single 64-bit atomic (doubles stored as IEEE 754 bit-
// pattern). There is no seqlock. Coherence between the fields that MUST agree
// comes from a directional protocol instead: the writer stores the value
// first (relaxed) and the KEY field last (release), so a reader that
// acquire-loads the key is guaranteed to see the value anchored for it.
//
//   pair                              key field
//   beat_origin_ntp + bpm             bpm
//   is_playing_at_ntp + is_playing    is_playing
//
// The meter is the one two-part value that is NOT a pair: numerator and
// denominator travel packed in one word (packMeter), so a reader can never
// see a 7 over the old 4.
//
// That contract is easy to get wrong, so nobody implements it in place:
// writers go through the mutators on the struct (setTempo / retempo /
// setOrigin / setTransport / setFlag / copyFrom — test_clock_state.cpp) and
// readers call readClockworkClock() below, which is the only reader anyone needs.
// The JS twins are in js/lib/clockwork_clock_protocol.js.
// The zero initialisers are required, not decoration. A std::atomic with no
// initialiser is INDETERMINATE under default-initialisation, and this struct is
// not only mapped over shared memory — ClockworkClock::Impl declares one as an
// ordinary member (`ownedState`), the private state a clock answers from before
// the engine binds it into the arena. Without these, a freshly constructed
// ClockworkClock reported whatever the stack held: isLinkEnabled() came back true
// on a build with no Link compiled in at all.
//
// Mapping a region and casting to this type is unaffected — nothing is
// constructed there, so nothing is written. initDefaults() below is still the
// way to get a *useful* state (it is the only thing that knows the default
// tempo); these merely guarantee that "not yet initialised" reads as zero
// rather than as garbage.
struct alignas(8) ClockworkClockState {
    std::atomic<uint64_t> bpm{0};                  // 0-7:  BPM as IEEE 754 bit-pattern
    std::atomic<uint64_t> beat_origin_ntp{0};      // 8-15: NTP seconds as bit-pattern
    std::atomic<uint64_t> is_playing_at_ntp{0};    // 16-23: NTP seconds as bit-pattern
    std::atomic<uint32_t> is_playing{0};           // 24-27: 0 = stopped, 1 = playing
    std::atomic<uint32_t> flags{0};                // 28-31: bit-packed session flags
    std::atomic<uint32_t> meter{0};                // 32-35: (num << 16) | den, see packMeter
    std::atomic<uint32_t> generation{0};           // 36-39: one more each time the grid (tempo, origin) moves

    // The meter as one word, so the two halves are read together. Both
    // halves fit 16 bits with room to spare (a meter is small integers).
    static uint32_t packMeter(int32_t num, int32_t den) {
        return (static_cast<uint32_t>(num) << 16u) | (static_cast<uint32_t>(den) & 0xFFFFu);
    }
    static int32_t meterNum(uint32_t packed) { return static_cast<int32_t>(packed >> 16u); }
    static int32_t meterDen(uint32_t packed) { return static_cast<int32_t>(packed & 0xFFFFu); }

    static void initDefaults(ClockworkClockState& s) {
        s.bpm.store(clockwork::doubleToBits(clockwork::kDefaultBpm), std::memory_order_relaxed);
        s.beat_origin_ntp.store(0u,                  std::memory_order_relaxed);
        s.is_playing_at_ntp.store(0u,                std::memory_order_relaxed);
        s.is_playing.store(0u,                       std::memory_order_relaxed);
        s.flags.store(0u,                            std::memory_order_relaxed);
        s.meter.store(packMeter(4, 4),               std::memory_order_relaxed);
        s.generation.store(0u,                       std::memory_order_relaxed);
    }

    // ── The write protocol, once ──────────────────────────────────────────
    // Tempo below 1 is clamped: requestBeatAtTime / timeAtBeat divide by it,
    // and 0 or NaN would write ±inf/NaN into the origin and poison every read.
    static double clampBpm(double bpm) { return bpm >= 1.0 ? bpm : 1.0; }

    // Publish a grid: origin first, then the tempo with release, so a reader
    // that acquire-loads the tempo sees the origin anchored for it; then the
    // generation, last, so a reader that loaded it first and sees it unchanged
    // afterwards read no newer grid than it thinks.
    void setTempo(double bpmIn, double originNtp) {
        beat_origin_ntp.store(clockwork::doubleToBits(originNtp), std::memory_order_relaxed);
        bpm.store(clockwork::doubleToBits(clampBpm(bpmIn)), std::memory_order_release);
        generation.fetch_add(1u, std::memory_order_release);
    }

    // Change the tempo without moving the beat playing at `nowNtp`
    // (clockwork::retempoOrigin). Returns that beat.
    double retempo(double bpmIn, double nowNtp) {
        const double newBpm = clampBpm(bpmIn);
        const double oldBpm = clockwork::bitsToDouble(bpm.load(std::memory_order_relaxed));
        const double origin = clockwork::bitsToDouble(beat_origin_ntp.load(std::memory_order_relaxed));
        const double held   = (origin != 0.0 && oldBpm >= 1.0)
                            ? clockwork::beatAt(nowNtp, origin, oldBpm) : 0.0;
        setTempo(newBpm, clockwork::retempoOrigin(origin, oldBpm, newBpm, nowNtp));
        return held;
    }

    // Move the grid under the current tempo (requestBeatAtTime and friends).
    void setOrigin(double originNtp) {
        beat_origin_ntp.store(clockwork::doubleToBits(originNtp), std::memory_order_relaxed);
        generation.fetch_add(1u, std::memory_order_release);
    }

    // Timestamp first, then the flag with release — same shape as setTempo.
    void setTransport(bool playing, double atNtp) {
        is_playing_at_ntp.store(clockwork::doubleToBits(atNtp), std::memory_order_relaxed);
        is_playing.store(playing ? 1u : 0u, std::memory_order_release);
    }

    // One bit, siblings untouched.
    void setFlag(uint32_t mask, bool on) {
        if (on) flags.fetch_or(mask,   std::memory_order_relaxed);
        else    flags.fetch_and(~mask, std::memory_order_relaxed);
    }

    // The meter, as one word. Validation is the caller's (ClockworkClock::setMeter
    // asks clockwork_timeline_meter_valid); the grid is untouched, because bars are
    // counted from beat 0 whatever the meter.
    void setMeter(int32_t num, int32_t den) {
        meter.store(packMeter(num, den), std::memory_order_relaxed);
    }

    // A coherent copy: read `src` under its protocol, write here under ours.
    inline void copyFrom(const ClockworkClockState& src);
};

// A coherent snapshot of the session clock.
//
// This exists because the DSP consumes the clock too, and a DSP is written by
// someone who should not have to know the ordering rules above. Read the key
// field with acquire, then the value it anchors: that is the whole protocol,
// implemented once, here.
struct ClockworkClockSnapshot {
    double   bpm;                // beats per minute
    double   beat_origin_ntp;    // NTP seconds at which beat 0 occurred
    double   is_playing_at_ntp;  // NTP seconds the transport last changed
    bool     is_playing;
    uint32_t flags;              // SC_FLAG_*
    int32_t  meter_num;          // how quarter-note beats group into bars
    int32_t  meter_den;
    // The grid's generation when this was read: one more each time the tempo
    // or origin is written. A follower that keeps its own copy of the grid (a
    // language runtime's beats, a MIDI clock) re-reads when it has moved on,
    // rather than comparing doubles or waiting for a notification.
    uint32_t generation;

    // The beat at an NTP instant, under this snapshot.
    double beatAt(double ntpSeconds) const {
        return clockwork::beatAt(ntpSeconds, beat_origin_ntp, bpm);
    }
};

inline ClockworkClockSnapshot readClockworkClock(const ClockworkClockState* s) {
    ClockworkClockSnapshot out{};
    if (!s) {
        out.bpm = clockwork::kDefaultBpm;
        out.meter_num = 4;
        out.meter_den = 4;
        return out;
    }
    // The generation before the grid: writers bump it after, so the grid read
    // is never older than the generation says.
    out.generation = s->generation.load(std::memory_order_acquire);
    // bpm is the key for beat_origin_ntp; is_playing is the key for its
    // timestamp. Acquire first, then the anchored value.
    out.bpm = clockwork::bitsToDouble(s->bpm.load(std::memory_order_acquire));
    out.beat_origin_ntp =
        clockwork::bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
    out.is_playing = s->is_playing.load(std::memory_order_acquire) != 0u;
    out.is_playing_at_ntp =
        clockwork::bitsToDouble(s->is_playing_at_ntp.load(std::memory_order_relaxed));
    out.flags = s->flags.load(std::memory_order_relaxed);
    const uint32_t meter = s->meter.load(std::memory_order_relaxed);
    out.meter_num = ClockworkClockState::meterNum(meter);
    out.meter_den = ClockworkClockState::meterDen(meter);
    return out;
}

inline void ClockworkClockState::copyFrom(const ClockworkClockState& src) {
    // A faithful copy, not a sanitising one: the bits travel as they are (a
    // region that was never initDefaults'd copies as the zeros it holds), in
    // the same value-then-key order the mutators use.
    const ClockworkClockSnapshot in = readClockworkClock(&src);
    beat_origin_ntp.store(clockwork::doubleToBits(in.beat_origin_ntp), std::memory_order_relaxed);
    bpm.store(clockwork::doubleToBits(in.bpm), std::memory_order_release);
    setTransport(in.is_playing, in.is_playing_at_ntp);
    flags.store(in.flags, std::memory_order_relaxed);
    setMeter(in.meter_num, in.meter_den);
    generation.store(in.generation, std::memory_order_release);   // a mirror counts as its source does
}

// Bit positions inside ClockworkClockState::flags. Single atomic uint32 so
// readers can snapshot all flags in one load; writers use fetch_or /
// fetch_and to mutate individual bits without stomping siblings.
constexpr uint32_t SC_FLAG_LINK_ENABLED         = 1u << 0u;
constexpr uint32_t SC_FLAG_START_STOP_SYNC      = 1u << 1u;
constexpr uint32_t SC_FLAG_LINK_AUDIO_PUBLISH   = 1u << 2u;

#endif /* CLOCKWORK_CLOCK_STATE_H */
