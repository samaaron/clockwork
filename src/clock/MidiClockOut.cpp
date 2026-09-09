// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * MidiClockOut.cpp — see MidiClockOut.h.
 */
#include "clock/MidiClockOut.h"

#include "clock/ClockworkClock.h"
#include "clock/Timeline.h"
#include "clock/clock_math.h"
#include "clockwork_clock.h"

#include <algorithm>

// The sink's shape is the MIDI profile's (CLOCKWORK_MIDI_SINK_CLASSES in
// memory_profile.h): a few hundred tiny cells for ticks and notes, one wide
// one for sysex. 0 at open takes it. A full sink drops and counts.
static constexpr uint32_t kSinkCapacity = 0;

std::atomic<MidiClockOut*> g_active_midi_clock_out{nullptr};

ClockworkSink midi_clock_sink_for_port(const std::string& port) {
    // "*" and "" mean every port, which cannot be one sink. Resolved at send
    // time instead, where listing the open sinks allocates nothing.
    if (port.empty() || port == "*") return CLOCKWORK_SINK_NONE;
    return midi_sink_for_port(port);
}

ClockworkSink midi_sink_for_port(const std::string& port) {
    // Reuse the sink already open on this port. Opening a second one would
    // double every tick on the wire.
    ClockworkSink open[MidiClockOut::kMaxFanout];
    const uint32_t n = clockwork_sink_list(open, MidiClockOut::kMaxFanout);
    for (uint32_t i = 0; i < n && i < MidiClockOut::kMaxFanout; ++i) {
        if (clockwork_sink_kind(open[i]) != kClockworkSinkMidi) continue;
        const char* target = clockwork_sink_target(open[i]);
        if (target && port == target) return open[i];
    }
    // CLOCKWORK_SINK_NONE if this build has no MIDI endpoint — the caller records
    // the ticks anyway and they are counted as dropped, so a build without
    // MIDI reports the loss rather than quietly reshaping the burst.
    return clockwork_sink_open(kClockworkSinkMidi, port.c_str(), kSinkCapacity);
}

void MidiClockOut::reset() {
    for (Follower& f : mFollowers) f = Follower{};
    mFollowFloorNtp = 0.0;
    mResetPending.store(true, std::memory_order_release);
}

bool MidiClockOut::push(double atNtp, ClockworkSink sink, uint8_t byte) {
    const uint32_t head = mHead.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % kSlots;
    if (next == mTail.load(std::memory_order_acquire)) {
        mDropped.fetch_add(1, std::memory_order_relaxed);   // ring full
        return false;
    }
    Slot& slot = mRing[head % kSlots];
    slot.atNtp = atNtp;
    slot.sink  = sink;
    slot.byte  = byte;
    mHead.store(next, std::memory_order_release);
    return true;
}

void MidiClockOut::onBeat(ClockworkClock& clock, const std::string& port,
                          double durationSeconds) {
    const double now = clock.now();
    // Resolved HERE, on the command thread, because opening a sink allocates
    // and may touch the device. The render thread only ever sends.
    const ClockworkSink sink = midi_clock_sink_for_port(port);

    for (int64_t i = 0; i < kPulsesPerBeat; ++i)
        push(now + durationSeconds * static_cast<double>(i)
                       / static_cast<double>(kPulsesPerBeat),
             sink, kClockTick);
}

// ── Followers ────────────────────────────────────────────────────────────────

MidiClockOut::Follower* MidiClockOut::followerFor(const std::string& port) {
    for (Follower& f : mFollowers)
        if (f.active && f.port == port) return &f;
    return nullptr;
}

bool MidiClockOut::follow(const std::string& port, const std::string& timeline,
                          int timelineId, ClockworkSink sink) {
    Follower* f = followerFor(port);
    if (!f) {
        for (Follower& free : mFollowers)
            if (!free.active) { f = &free; break; }
        if (!f) return false;
    }
    // Re-targeting starts afresh: the next index belongs to the old grid.
    *f = Follower{};
    f->active     = true;
    f->port       = port;
    f->timeline   = timeline;
    f->timelineId = timelineId;
    f->sink       = sink;
    f->next       = CLOCKWORK_MIDI_CLOCK_OUT_UNSTARTED;
    return true;
}

bool MidiClockOut::unfollow(const std::string& port) {
    Follower* f = followerFor(port);
    if (!f) return false;
    *f = Follower{};
    return true;
}

std::vector<MidiClockOut::FollowerInfo> MidiClockOut::followers() const {
    std::vector<FollowerInfo> out;
    for (const Follower& f : mFollowers)
        if (f.active) out.push_back({f.port, f.timeline});
    return out;
}

void MidiClockOut::tick(ClockworkClock& clock, double nowNtp) {
    // One tick's batch, gathered then recorded in time order: followers on
    // different grids interleave, and generate() stops at the first slot
    // beyond its horizon, so a slot recorded out of order would hold every
    // one behind it. Pulses plus one transport byte per follower, at most.
    struct Pending {
        double  atNtp;
        ClockworkSink sink;
        uint8_t byte;
    };
    Pending  batch[kMaxFollowers * (kMaxPulsesPerTick + 1)];
    uint32_t n = 0;
    const double floor = mFollowFloorNtp;

    for (Follower& f : mFollowers) {
        if (!f.active) continue;
        const clockwork::Timeline tl = clock.timeline(f.timelineId);

        // Transport: one Start or Stop per transition, at the time the
        // timeline reports for it. The first tick adopts the state silently —
        // following a running transport is not a transition in it. Held to
        // the floor because the transition is in the past and pulses up to
        // the floor are already recorded; it goes out before the next one.
        const int8_t playing = tl.playing ? 1 : 0;
        if (f.playing < 0) {
            f.playing = playing;
        } else if (playing != f.playing) {
            const double at = tl.transition_ntp != 0.0 ? tl.transition_ntp : nowNtp;
            batch[n++] = { std::max(at, floor), f.sink, playing ? kStart : kStop };
            f.playing = playing;
        }

        uint32_t count = 0;
        const int64_t first = clockwork_midi_clock_out_due(&tl, f.next, floor, nowNtp,
                                                     kFollowHorizonSeconds,
                                                     kMaxPulsesPerTick, &count);
        for (uint32_t i = 0; i < count; ++i) {
            // A pulse a tempo rise moved behind the floor is held to it: a
            // hair late, in order, rather than out of order and on time.
            const double at = clockwork_midi_clock_out_pulse_ntp(&tl, first + static_cast<int64_t>(i));
            batch[n++] = { std::max(at, floor), f.sink, kClockTick };
        }
        f.next = first + count;
    }

    // Time order; a transport byte ahead of a pulse at the same instant, as
    // a receiver expects Start before the pulse it starts on.
    std::sort(batch, batch + n, [](const Pending& a, const Pending& b) {
        if (a.atNtp != b.atNtp) return a.atNtp < b.atNtp;
        return a.byte > b.byte;   // 0xFA / 0xFC before 0xF8
    });
    for (uint32_t i = 0; i < n; ++i) {
        push(batch[i].atNtp, batch[i].sink, batch[i].byte);
        if (batch[i].atNtp > mFollowFloorNtp) mFollowFloorNtp = batch[i].atNtp;
    }
}

// ── Render thread ────────────────────────────────────────────────────────────

void MidiClockOut::generate(double nowNtp) {
    if (mResetPending.exchange(false, std::memory_order_acq_rel)) {
        // Consume-side clear: everything recorded so far is abandoned.
        mTail.store(mHead.load(std::memory_order_acquire), std::memory_order_release);
        return;
    }

    const double horizonNtp = nowNtp + kLookaheadSeconds;
    uint32_t tail = mTail.load(std::memory_order_relaxed);
    const uint32_t head = mHead.load(std::memory_order_acquire);

    while (tail != head) {
        const Slot& slot = mRing[tail % kSlots];
        if (slot.atNtp > horizonNtp)
            break;   // ring order ~ time order; the horizon catches up next block

        // The tick carries its own time all the way to the endpoint, which
        // honours it as close to the wire as its platform allows.
        const int64_t when = clockwork::ntpToOscTimetag(slot.atNtp);
        const uint8_t byte = slot.byte;

        if (slot.sink != CLOCKWORK_SINK_NONE) {
            if (clockwork_sink_send(slot.sink, &byte, 1, when))
                mSent.fetch_add(1, std::memory_order_relaxed);
            else
                mDropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Every open MIDI sink. Listing writes into this stack array and
            // allocates nothing, which is what makes the fan-out RT-safe.
            ClockworkSink open[kMaxFanout];
            const uint32_t n = clockwork_sink_list(open, kMaxFanout);
            uint32_t matched = 0;
            for (uint32_t i = 0; i < n && i < kMaxFanout; ++i) {
                if (clockwork_sink_kind(open[i]) != kClockworkSinkMidi) continue;
                ++matched;
                if (clockwork_sink_send(open[i], &byte, 1, when))
                    mSent.fetch_add(1, std::memory_order_relaxed);
                else
                    mDropped.fetch_add(1, std::memory_order_relaxed);
            }
            // A burst aimed at "every port" when there is no port is still a
            // tick that did not go out. Counted, because otherwise a build
            // with no MIDI endpoint — or a session with nothing plugged in —
            // generates 24 ticks a beat that vanish with every counter at
            // zero, which is indistinguishable from working.
            if (matched == 0)
                mDropped.fetch_add(1, std::memory_order_relaxed);
        }
        tail = (tail + 1) % kSlots;
    }
    mTail.store(tail, std::memory_order_release);
}
