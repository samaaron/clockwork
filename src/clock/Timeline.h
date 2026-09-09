// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * Timeline.h — a beat grid as a value.
 *
 * One timeline is five numbers and a few flags: a tempo, one (beat, time)
 * anchor the grid passes through, and whether the source is playing and has
 * actually placed its beat origin. Times are NTP seconds. Given that, every
 * beat<->time question is arithmetic and needs no clock, no lock and no
 * source — which is why ClockworkClock::timeline(id) hands one out by value and
 * the OSC verbs answer from the copy.
 *
 * The layout and the arithmetic live in Rust (rust/clockwork-clock, `Timeline`);
 * this is the same struct with methods, so C++ callers write t.beatAt(now)
 * instead of clockwork_timeline_beat_at(&t, now). The Rust side is the twin of
 * clock_math.h's readClockworkClock and js/lib/clock_math.js; the ABI functions are
 * the single implementation all three agree with.
 */
#pragma once

#include "shared_memory.h"
#include "clockwork_clock.h"

namespace clockwork {

struct Timeline : ClockworkTimeline {
    // The grid for a name nothing holds: 60 BPM from time 0, stopped,
    // unanchored. Every question still has an answer.
    static Timeline placeholder(int32_t id = -1) {
        Timeline t;
        static_cast<ClockworkTimeline&>(t) = clockwork_timeline_placeholder(id);
        return t;
    }

    // The Link timeline (id 0) is the clock state: one coherent read of it.
    // The grid always exists whether or not the transport is running, and
    // beat 0 is at the origin — so the anchor is (0, origin). Everyone who
    // answers from the session clock — ClockworkClock::timeline(0), a hosted
    // plugin's transport — reads it through this, so there is one reading.
    static Timeline fromClockState(const ClockworkClockState* s) {
        const ClockworkClockSnapshot c = readClockworkClock(s);
        Timeline t = placeholder(0);
        t.bpm            = c.bpm;
        t.anchor_beat    = 0.0;
        t.anchor_ntp     = c.beat_origin_ntp;
        t.transition_ntp = c.is_playing_at_ntp;
        t.playing        = c.is_playing ? 1 : 0;
        t.anchored       = 1;
        t.meter_num      = c.meter_num;
        t.meter_den      = c.meter_den;
        return t;
    }

    double beatAt(double ntp) const { return clockwork_timeline_beat_at(this, ntp); }
    double timeAtBeat(double beat) const { return clockwork_timeline_time_at_beat(this, beat); }
    double phaseAt(double ntp, double quantum) const {
        return clockwork_timeline_phase_at(this, ntp, quantum);
    }
    // Bars: quarter-note beats per bar from the meter, bar 0 at beat 0.
    double beatsPerBar() const { return clockwork_timeline_beats_per_bar(this); }
    double barAt(double ntp) const { return clockwork_timeline_bar_at(this, ntp); }
    double beatInBarAt(double ntp) const { return clockwork_timeline_beat_in_bar_at(this, ntp); }
};

static_assert(sizeof(Timeline) == 56, "Timeline is 4 doubles + 5 int32 + tail padding");

} // namespace clockwork
