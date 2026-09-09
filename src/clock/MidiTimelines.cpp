// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * MidiTimelines.cpp — the C++ face of rust/clockwork-clock's follower registry.
 *
 * Thin by design: every call is "stamp now, cross the ABI, fire the callback
 * if the listing moved". The estimator, the slot table and the staleness
 * rule are on the other side and are tested there.
 */
#include "clock/MidiTimelines.h"
#include "clock/clock_math.h"
#include "memory_profile.h"

#include <cstring>
#include <utility>

namespace {
uint32_t len(const char* s) { return s ? static_cast<uint32_t>(std::strlen(s)) : 0u; }

// One listing row, copied out of the registry while it holds its lock.
extern "C" void copyRow(void* ctx, const ClockworkTimelineInfo* row) {
    ClockworkClock::TimelineInfo info;
    info.name.assign(row->name, row->name_len);
    info.raw.assign(row->raw, row->raw_len);
    info.bpm      = row->bpm;
    info.clocking = row->clocking != 0;
    info.stale    = row->stale != 0;
    info.primary  = row->primary != 0;
    static_cast<std::vector<ClockworkClock::TimelineInfo>*>(ctx)->push_back(std::move(info));
}
}  // namespace

MidiTimelines::MidiTimelines(ClockworkClock& clock)
    : mClock(clock), mRegistry(clockwork_midi_timelines_new(SC_MAX_TIMELINES)) {}

MidiTimelines::~MidiTimelines() { clockwork_midi_timelines_free(mRegistry); }

int MidiTimelines::claimMidiTimeline(const char* normalized, const char* raw) {
    int32_t changed = 0;
    const int id = clockwork_midi_timelines_claim(mRegistry, normalized, len(normalized), raw, len(raw),
                                            wallClockNTP(), &changed);
    notify(changed);
    return id;
}

void MidiTimelines::freeMidiTimeline(int id) {
    notify(clockwork_midi_timelines_release(mRegistry, id));
}

int MidiTimelines::resolveTimeline(const char* name) const {
    return clockwork_midi_timelines_resolve(mRegistry, name, len(name));
}

int MidiTimelines::resolveOrClaimTimeline(const char* name) {
    int32_t changed = 0;
    const int id = clockwork_midi_timelines_resolve_or_claim(mRegistry, name, len(name), wallClockNTP(),
                                                       &changed);
    notify(changed);
    return id;
}

void MidiTimelines::midiTimelinePulse(int id, uint64_t tsUs) {
    notify(clockwork_midi_timelines_pulse(mRegistry, id, tsUs, wallClockNTP()));
}

void MidiTimelines::setMidiTimelineTempo(int id, double bpm) {
    notify(clockwork_midi_timelines_set_tempo(mRegistry, id, bpm, wallClockNTP()));
}

void MidiTimelines::setMidiTimelineTransport(int id, int kind, double beat) {
    notify(clockwork_midi_timelines_transport(mRegistry, id, kind, beat, wallClockNTP()));
}

bool MidiTimelines::setMidiTimelineMeter(int id, int num, int den) {
    // The listing does not carry the meter: nothing to notify.
    return clockwork_midi_timelines_set_meter(mRegistry, id, num, den) != 0;
}

void MidiTimelines::tickMidiStaleness() {
    notify(clockwork_midi_timelines_tick_stale(mRegistry, wallClockNTP()));
}

clockwork::Timeline MidiTimelines::timeline(int id) const {
    clockwork::Timeline t = clockwork::Timeline::placeholder(-1);
    clockwork_midi_timelines_timeline(mRegistry, id, &t);
    return t;
}

bool MidiTimelines::timelineRt(int id, clockwork::Timeline& out) const {
    return clockwork_midi_timelines_timeline_rt(mRegistry, id, &out) != 0;
}

std::vector<ClockworkClock::TimelineInfo> MidiTimelines::listTimelines() const {
    std::vector<ClockworkClock::TimelineInfo> out;
    ClockworkClock::TimelineInfo link;
    link.name     = "link";
    link.raw      = "link";
    link.bpm      = mClock.getBpm();
    link.clocking = true;       // Link is always live
    out.push_back(std::move(link));

    clockwork_midi_timelines_each(mRegistry, &out, &copyRow);
    return out;
}

void MidiTimelines::setTimelinesChangedCallback(std::function<void()> cb) {
    mTimelinesChangedCb = std::move(cb);
}
