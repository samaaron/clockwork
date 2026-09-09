// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * MidiTimelines.h — the MIDI-clock follower-timeline registry, from C++.
 *
 * A fixed K-slot registry of midi:<port> follower timelines, independent of
 * Ableton Link. The MIDI subsystem feeds pulses / transport per port; OSC
 * clients read the timelines via /clockwork/clock/midi:<port>/*. Slot
 * assignment, tempo estimation, primary selection and staleness all live in
 * Rust (rust/clockwork-clock, `Registry`); this is the C++ face of that handle:
 * it supplies "now" (wall-clock NTP), sizes the registry to the platform's
 * SC_MAX_TIMELINES, prepends the Link row to the listing, and fires the
 * timelines-changed callback when the registry says the listing moved.
 *
 * Timeline id: 0 = Link (routes to the ClockworkClock& reads); 1..K = midi slots.
 * Every time is NTP seconds — the same domain the Link timeline answers in.
 * The handle is internally locked; there is no mutex here.
 */
#pragma once

#include "clock/ClockworkClock.h"
#include "clock/Timeline.h"

#include <cstdint>
#include <functional>
#include <vector>

struct ClockworkMidiTimelines;

class MidiTimelines {
public:
    explicit MidiTimelines(ClockworkClock& clock);
    ~MidiTimelines();

    MidiTimelines(const MidiTimelines&) = delete;
    MidiTimelines& operator=(const MidiTimelines&) = delete;

    int  claimMidiTimeline(const char* normalized, const char* raw);
    void freeMidiTimeline(int id);
    int  resolveTimeline(const char* name) const;
    int  resolveOrClaimTimeline(const char* name);

    void midiTimelinePulse(int id, uint64_t tsUs);
    void setMidiTimelineTempo(int id, double bpm);
    void setMidiTimelineTransport(int id, int kind, double beat);
    bool setMidiTimelineMeter(int id, int num, int den);
    void tickMidiStaleness();

    // The grid for a midi slot, or the placeholder when nothing holds `id`.
    // (id 0 = Link is answered by ClockworkClock, not here.)
    clockwork::Timeline timeline(int id) const;

    // The same, without the lock: the copy every mutator publishes, read
    // under a seqlock. AUDIO THREAD. An unheld slot reads as the id -1
    // placeholder; false (out untouched) for an id past the registry or a
    // writer caught mid-way — keep the last snapshot then.
    bool timelineRt(int id, clockwork::Timeline& out) const;

    std::vector<ClockworkClock::TimelineInfo> listTimelines() const;
    void setTimelinesChangedCallback(std::function<void()> cb);

private:
    // `changed` is the registry's answer to "did the listing move"; 1 fires
    // the callback. Called outside the registry lock, so the callback may
    // read the registry again.
    void notify(int changed) { if (changed && mTimelinesChangedCb) mTimelinesChangedCb(); }

    ClockworkClock&              mClock;
    ClockworkMidiTimelines*      mRegistry;
    std::function<void()>  mTimelinesChangedCb;
};
