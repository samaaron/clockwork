// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * host_transport.h — a host's own transport, taken by the timeline.
 *
 * The four things a host says of it (clockwork_host_transport.h), each as the
 * arithmetic on the clock state's own mutators, for the audio thread: nothing
 * here allocates or takes a lock. Each answers what it moved, and moves
 * nothing when what the host says is there already, so a host that says its
 * transport again with every render moves the grid, and every follower with
 * it, only when it has changed.
 */
#pragma once

#include "clock/Timeline.h"
#include "clock/clock_math.h"
#include "clockwork_host_transport.h"
#include "shared_memory.h"

#include <cmath>
#include <cstdint>

namespace clockwork {

// What following it moved.
enum : uint32_t {
    kHostMovedTempo     = 1u << 0u,
    kHostMovedMeter     = 1u << 1u,
    kHostMovedTransport = 1u << 2u,
    kHostMovedGrid      = 1u << 3u,   // its bars or its beats, under the same tempo
};

// The least time between two tellings of what the host moved
// (/clockwork/clock/notify/…): a host sweeping its tempo moves the grid every
// block, and the clients hear of it this often, the last move always.
inline constexpr double kHostNotifySeconds = 0.02;

// One thing a host said, and the frame of its audio it is true at.
struct HostWord {
    enum Kind : uint32_t { Tempo, Meter, Playing, Beat };
    Kind     kind = Tempo;
    uint32_t how = CLOCKWORK_BEAT_REQUEST;   // Beat
    int32_t  num = 0, den = 0;               // Meter
    bool     playing = false;                // Playing
    double   value = 0.0;                    // Tempo: BPM. Beat: quarter notes
    uint64_t frame = 0;
};

// A tempo a host can say: a number above 0. The clock's own limit is the
// state's (ClockworkClockState::setTempo), as it is for a client's.
inline bool hostTempoValid(double bpm) { return std::isfinite(bpm) && bpm > 0.0; }

// Its tempo is the timeline's from `atNtp`, the beat playing then held.
inline uint32_t hostTempo(ClockworkClockState& s, double bpm, double atNtp) {
    if (!hostTempoValid(bpm)) return 0u;
    // One under the clock's least is the least: said again, it is there already.
    if (ClockworkClockState::clampBpm(bpm) == readClockworkClock(&s).bpm) return 0u;
    s.retempo(bpm, atNtp);
    return kHostMovedTempo;
}

inline uint32_t hostMeter(ClockworkClockState& s, int32_t num, int32_t den) {
    if (!clockwork_timeline_meter_valid(num, den)) return 0u;
    const ClockworkClockSnapshot now = readClockworkClock(&s);
    if (num == now.meter_num && den == now.meter_den) return 0u;
    s.setMeter(num, den);
    return kHostMovedMeter;
}

inline uint32_t hostPlaying(ClockworkClockState& s, bool playing, double atNtp) {
    if (playing == readClockworkClock(&s).is_playing) return 0u;
    s.setTransport(playing, atNtp);
    return kHostMovedTransport;
}

// Its beat is `beat` at `atNtp`, a frame of audio at `rate` frames a second.
// Followed while the timeline plays, and only when the grid is more than half
// a frame from it.
inline uint32_t hostBeat(ClockworkClockState& s, double beat, uint32_t how, double atNtp, double rate) {
    const bool force = how == CLOCKWORK_BEAT_FORCE;
    if ((!force && how != CLOCKWORK_BEAT_REQUEST) || !std::isfinite(beat) || !(rate > 0.0)) return 0u;
    const ClockworkClockSnapshot now = readClockworkClock(&s);
    const double bar = now.meter_num * 4.0 / now.meter_den;   // quarter notes
    if (!now.is_playing || !(now.bpm >= 1.0) || (!force && !(bar > 0.0))) return 0u;
    const double hostAt = force ? beat : wrapPhase(beat, bar);
    if (now.beat_origin_ntp == 0.0) {
        // Nobody has placed the grid: it starts where the host is.
        s.setOrigin(originFor(hostAt, atNtp, now.bpm));
        return kHostMovedGrid;
    }
    // How far on the host is from the grid: by force outright, by request the shorter way round the bar.
    double apart = hostAt - (force ? now.beatAt(atNtp) : wrapPhase(now.beatAt(atNtp), bar));
    if (!force) {
        apart = wrapPhase(apart, bar);
        if (apart > bar * 0.5) apart -= bar;
    }
    const double seconds = apart * 60.0 / now.bpm;
    if (std::abs(seconds) * rate <= 0.5) return 0u;
    s.setOrigin(now.beat_origin_ntp - seconds);
    return kHostMovedGrid;
}

// The timeline in `s` follows `w`, true at `atNtp`: what moved, as kHostMoved*.
inline uint32_t followHost(ClockworkClockState& s, const HostWord& w, double atNtp, double rate) {
    switch (w.kind) {
        case HostWord::Tempo:   return hostTempo(s, w.value, atNtp);
        case HostWord::Meter:   return hostMeter(s, w.num, w.den);
        case HostWord::Playing: return hostPlaying(s, w.playing, atNtp);
        case HostWord::Beat:    return hostBeat(s, w.value, w.how, atNtp, rate);
    }
    return 0u;
}

}  // namespace clockwork
