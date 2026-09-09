// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * plugin_transport.h — the musical position of one block, from a timeline.
 *
 * Both adapters tell their plugin where a block stands in the music — VST3
 * through ProcessContext, CLAP through clap_event_transport — and a
 * tempo-synced delay or an arpeggiator in either format is only as right as
 * that answer. Each format has its own struct, its own fixed point and its
 * own flags, but the position itself is one calculation on one clockwork::Timeline
 * snapshot, made here, so the two formats cannot disagree about the beat,
 * the bar or the meter.
 *
 * Beats are quarter notes whatever the meter (the unit every tempo, anchor
 * and host speaks); the meter only says where the bar lines fall, and bar 0
 * starts at beat 0 (clockwork_clock.h). The arithmetic is rust/clockwork-clock's.
 */
#pragma once

#include "clock/Timeline.h"
#include "clock/clock_math.h"
#include "shared_memory.h"

#include <cstdint>

namespace clockwork_plugin {

// The grid a block renders against: the timeline the host bound the plugin
// to for this block (plugin_set_timeline — a track following a midi
// timeline), else the session clock (plugin_set_clock), else nothing, and
// the plugin runs free. False when there is nothing.
inline bool blockTimeline(const ClockworkTimeline* bound, const ClockworkClockState* clock,
                          clockwork::Timeline& out) {
    if (bound) {
        static_cast<ClockworkTimeline&>(out) = *bound;
        return true;
    }
    if (clock) {
        out = clockwork::Timeline::fromClockState(clock);
        return true;
    }
    return false;
}

struct BlockTransport {
    double  bpm;
    bool    playing;
    double  beat;           // quarter notes at the block's first frame
    double  bar;            // 0-based, whole-valued (negative before beat 0)
    double  barStartBeat;   // the beat the current bar began on
    int32_t meterNum;
    int32_t meterDen;
};

// Where `block_time` — the block's OSC timetag — falls on `t`. A caller with
// no block time (a test, a host with no clock of its own) passes 0 and the
// block is placed at the timeline's anchor, which for the session clock is
// beat 0: the beat is then the anchor's rather than "now", so a plugin
// placing an event inside the block still lands it where the host intended.
inline BlockTransport blockTransport(const clockwork::Timeline& t, int64_t block_time) {
    const double ntp = block_time != 0 ? clockwork::oscTimetagToNtp(block_time) : t.anchor_ntp;
    BlockTransport b {};
    b.bpm          = t.bpm;
    b.playing      = t.playing != 0;
    b.beat         = t.beatAt(ntp);
    b.bar          = t.barAt(ntp);
    b.barStartBeat = b.bar * t.beatsPerBar();
    b.meterNum     = t.meter_num;
    b.meterDen     = t.meter_den;
    return b;
}

}  // namespace clockwork_plugin
