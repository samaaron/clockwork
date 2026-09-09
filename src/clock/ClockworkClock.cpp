// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * ClockworkClock.cpp — shared NTP-domain getters + beat math.
 *
 * Methods that touch Link or that vary by platform (mutators, Link-aware
 * getters, Link-clock RPC, audio-thread Link Audio paths) live in the
 * platform .cpp files. This file holds only the pieces that are
 * identical on native and WASM: pure reads of the ClockworkClockState
 * atomics + the NTP-domain beat math composed from them.
 */
#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"
#include "shared_memory.h"
#include "shm_scope_stream.hpp"  // g_engine_frames (stream anchor)

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

using clockwork::bitsToDouble;

std::atomic<ClockworkClock*> g_active_clockwork_clock{nullptr};

// ── Sample clock (sample position ↔ wall-clock DAC time) ───────────────

void ClockworkClock::bindSampleClockToShm(uint8_t* region) {
    mSampleClockRegion = region;
}

void ClockworkClock::advanceEngineFrames(double samplePosition) {
    g_engine_frames.store(static_cast<uint64_t>(samplePosition),
                          std::memory_order_relaxed);
}

uint64_t ClockworkClock::engineFrames() const {
    return g_engine_frames.load(std::memory_order_relaxed);
}

// Seqlock writer: odd seq, release fence (orders the odd store before the
// field stores for any reader that sees them), relaxed atomic field stores,
// even seq with release. Fields are atomics so no read tears; the seq guards
// cross-field consistency. Audio thread only — single writer by construction.
void ClockworkClock::publishSampleClock(double samplePosition, double sampleRate,
                                    double renderNtp,
                                    uint32_t outputLatencyFrames) {
    const uint64_t frames = static_cast<uint64_t>(samplePosition);
    // Streams anchor their per-block writes here whether or not a shm
    // region is bound (headless/unit contexts).
    g_engine_frames.store(frames, std::memory_order_relaxed);

    uint8_t* sc = mSampleClockRegion;
    if (!sc || sampleRate <= 0.0) return;
    auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(sc + SAMPLE_CLOCK_SEQ);
    auto* sr  = reinterpret_cast<std::atomic<uint32_t>*>(sc + SAMPLE_CLOCK_SAMPLE_RATE);
    auto* fr  = reinterpret_cast<std::atomic<uint64_t>*>(sc + SAMPLE_CLOCK_ENGINE_FRAMES);
    auto* nb  = reinterpret_cast<std::atomic<uint64_t>*>(sc + SAMPLE_CLOCK_DAC_NTP);
    auto* lat = reinterpret_cast<std::atomic<uint32_t>*>(sc + SAMPLE_CLOCK_OUT_LATENCY);

    // The anchor is speaker-time: render NTP plus the device output latency
    // (Link convention — "host time at speaker").
    const double dacNtp = renderNtp
        + static_cast<double>(outputLatencyFrames) / sampleRate;
    const uint64_t ntpBits = clockwork::doubleToBits(dacNtp);

    const uint32_t s = seq->load(std::memory_order_relaxed);
    seq->store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sr->store(static_cast<uint32_t>(sampleRate), std::memory_order_relaxed);
    fr->store(frames, std::memory_order_relaxed);
    nb->store(ntpBits, std::memory_order_relaxed);
    lat->store(outputLatencyFrames, std::memory_order_relaxed);
    seq->store(s + 2, std::memory_order_release);
}

// ── NTP-domain getters ──────────────────────────────────────────────────

double ClockworkClock::getBeatOriginNtp() const {
    const ClockworkClockState* s = state();
    if (!s) return 0.0;
    return bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
}

double ClockworkClock::getIsPlayingAtNtp() const {
    const ClockworkClockState* s = state();
    if (!s) return 0.0;
    return bitsToDouble(s->is_playing_at_ntp.load(std::memory_order_relaxed));
}

// tempo / transport read from the shared ClockworkClockState mirror — identical on
// every build. The mirror is written by setBpm/setIsPlaying and, on native, kept
// in sync with Link's converged value by Link's tempo/transport callbacks, so a
// relaxed atomic read here is RT-safe and consistent on every build.
double ClockworkClock::getBpm() const {
    const ClockworkClockState* s = state();
    if (!s) return 120.0;
    return bitsToDouble(s->bpm.load(std::memory_order_relaxed));
}

bool ClockworkClock::isPlaying() const {
    const ClockworkClockState* s = state();
    if (!s) return false;
    return s->is_playing.load(std::memory_order_relaxed) != 0u;
}

// ── NTP-domain beat math ────────────────────────────────────────────────
// Pure functions of (bpm, beat_origin). Independent atomic reads — no
// multi-field coherence guarantee. A caller that needs one coherent snapshot
// of several fields reads it with readClockworkClock(state()) (shared_memory.h).

double ClockworkClock::beatAtTime(double ntpSeconds, double quantum) const {
    (void)quantum;
    return clockwork::beatAt(ntpSeconds, getBeatOriginNtp(), getBpm());
}

double ClockworkClock::phaseAtTime(double ntpSeconds, double quantum) const {
    return clockwork::wrapPhase(beatAtTime(ntpSeconds, quantum), quantum);
}

double ClockworkClock::timeAtBeat(double beat, double quantum) const {
    (void)quantum;
    return clockwork::timeAtBeat(beat, getBeatOriginNtp(), getBpm());
}

// ── Cross-platform clock metrics ────────────────────────────────────────
// Reads the ClockworkClockState mirror directly. getBpm()/isPlaying() read the
// same atomics — they are defined once, above, on every build — so this is a
// matter of reading the fields it wants in one place rather than of avoiding
// a lock. The mirror is the SAB region on WASM and a Link-callback-synced
// private mirror on native, so the read is RT-safe on both. Fixed-point encoding matches the
// link_* clock slots so the same display formats apply.

void ClockworkClock::publishClockMetrics(PerformanceMetrics* m, double ntpNow, double quantum) {
    if (!m) return;

    const ClockworkClockState* s = state();
    if (!s) return;

    // Acquire-load bpm BEFORE origin: applyTempoChange stores the origin first
    // and releases bpm, so a new bpm here guarantees the matching origin.
    const double bpm = bitsToDouble(s->bpm.load(std::memory_order_acquire));
    const double beatOrigin = bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
    const bool playing = s->is_playing.load(std::memory_order_relaxed) != 0u;

    const double beat = (bpm > 0.0) ? clockwork::beatAt(ntpNow, beatOrigin, bpm) : 0.0;
    const double phase = clockwork::wrapPhase(beat, quantum);

    // The dashboard fields are 32-bit. A beat count from an origin at zero is
    // every beat since 1900 — ~8e9 at 120 BPM — and casting a double that does
    // not fit is undefined behaviour: x86 answers 0, arm64 saturates, UBSan
    // aborts. Saturate on purpose, on every platform.
    const auto u32 = [](double v) -> uint32_t {
        if (!(v > 0.0)) return 0u;
        return v >= 4294967295.0 ? UINT32_MAX : static_cast<uint32_t>(v);
    };
    m->clock_tempo_mbpm.store(u32(bpm * 1000.0 + 0.5), std::memory_order_relaxed);
    m->clock_beat_centi.store(u32(beat * 100.0), std::memory_order_relaxed);
    m->clock_phase_centi.store(u32(phase * 100.0), std::memory_order_relaxed);
    m->clock_playing.store(playing ? 1u : 0u, std::memory_order_relaxed);
}
