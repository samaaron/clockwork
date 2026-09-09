// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_track_timeline.cpp — how a follower timeline reaches a track's plugins.
 *
 * Two pieces of the engine's side of the plugin bridge, each pure enough to
 * pin without an engine: the seqlock a timeline snapshot crosses the segment
 * under (clock/timeline_mirror.h), and the rewrite that puts a resolved
 * timeline id on "/clockwork/track/timeline" before it is relayed
 * (TrackControl::resolveTimelineVerb). The bridge's half — the model
 * holding the binding, the block rendering against the mirrored slot — is
 * test_plugin_track.cpp's; the verb through a real bridge is
 * test_plugin_bridge.cpp's.
 */
#include <catch2/catch_test_macros.hpp>

#include "OscTestUtils.h"
#include "clock/timeline_mirror.h"
#include "native/TrackControl.h"
#include "clockwork_sys.h"

#include <cstring>
#include <string>
#include <vector>

TEST_CASE("timeline mirror: a snapshot crosses whole, and a writer mid-way is not read",
          "[plugin][track][clock][mirror]") {
    ClockworkTimelineMirrorSlot slot;
    ClockworkTimeline t = clockwork_timeline_placeholder(3);
    t.bpm = 133.7;
    t.anchor_beat = -2.5;
    t.anchor_ntp = 3'900'000'000.125;
    t.transition_ntp = 3'900'000'001.0;
    t.playing = 1;
    t.anchored = 1;
    t.meter_num = 13;
    t.meter_den = 16;

    // Never written: reads cleanly as the zero struct, id 0 — "nothing
    // there", which the track model treats like the id -1 placeholder.
    ClockworkTimeline out = clockwork_timeline_placeholder(9);
    REQUIRE(clockwork::timelineMirrorRead(slot, out));
    CHECK(out.id == 0);
    CHECK(out.bpm == 0.0);

    clockwork::timelineMirrorWrite(slot, t);
    REQUIRE(clockwork::timelineMirrorRead(slot, out));
    CHECK(std::memcmp(&out, &t, sizeof(ClockworkTimeline) - 4) == 0);   // the tail is padding
    CHECK(out.id == 3);
    CHECK(out.bpm == 133.7);
    CHECK(out.meter_num == 13);
    CHECK(out.meter_den == 16);

    // An odd sequence is a write in progress: the read gives up, and leaves
    // the caller's copy alone.
    slot.seq.fetch_add(1, std::memory_order_relaxed);
    out = clockwork_timeline_placeholder(9);
    CHECK_FALSE(clockwork::timelineMirrorRead(slot, out));
    CHECK(out.id == 9);
    slot.seq.fetch_add(1, std::memory_order_relaxed);
    REQUIRE(clockwork::timelineMirrorRead(slot, out));
    CHECK(out.id == 3);
}

namespace {

struct Seen {
    std::vector<std::string> names;
    int answer = 5;
    std::function<int(const char*)> resolver() {
        return [this](const char* n) { names.emplace_back(n); return answer; };
    }
};

osc_test::ParsedReply parse(const std::vector<uint8_t>& bytes) {
    return osc_test::parseReply(bytes.data(), static_cast<uint32_t>(bytes.size()));
}

bool same(const std::vector<uint8_t>& bytes, const osc_test::Packet& p) {
    return bytes.size() == p.size() && std::memcmp(bytes.data(), p.ptr(), p.size()) == 0;
}

}  // namespace

TEST_CASE("track/timeline: a midi name is resolved by the engine and the id rides along",
          "[plugin][track][clock][wire]") {
    Seen seen;

    // By track name, as code binds.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("track/timeline")) << "bass" << "midi:iac-1";
    const auto byName = b.end();
    auto out = TrackControl::resolveTimelineVerb(byName.ptr(), byName.size(), seen.resolver());
    auto r = parse(out);
    CHECK(r.address == CLOCKWORK_SYS("track/timeline"));
    CHECK(r.argCount() == 3);
    CHECK(r.argString(0) == "bass");
    CHECK(r.argString(1) == "midi:iac-1");
    CHECK(r.argInt(2) == 5);
    REQUIRE(seen.names == std::vector<std::string>{"midi:iac-1"});

    // By track id, as a panel binds; and a name the engine has no slot for
    // rides along as -1, which the bridge refuses.
    seen.answer = -1;
    osc_test::Builder b2;
    b2.begin(CLOCKWORK_SYS("track/timeline")) << 7 << "midi:nope";
    const auto byId = b2.end();
    r = parse(TrackControl::resolveTimelineVerb(byId.ptr(), byId.size(), seen.resolver()));
    CHECK(r.argInt(0) == 7);
    CHECK(r.argString(1) == "midi:nope");
    CHECK(r.argInt(2) == -1);
    CHECK(seen.names.size() == 2);
}

TEST_CASE("track/timeline: link, a query and other verbs pass through untouched",
          "[plugin][track][clock][wire]") {
    Seen seen;
    // "link" the bridge knows; nothing to resolve.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("track/timeline")) << "bass" << "link";
    const auto toLink = b.end();
    CHECK(same(TrackControl::resolveTimelineVerb(toLink.ptr(), toLink.size(), seen.resolver()), toLink));
    // The query: a track and no name.
    const auto query = osc_test::message(CLOCKWORK_SYS("track/timeline"), "bass");
    CHECK(same(TrackControl::resolveTimelineVerb(query.ptr(), query.size(), seen.resolver()), query));
    // Another verb entirely.
    const auto gain = osc_test::message(CLOCKWORK_SYS("track/gain"), 3);
    CHECK(same(TrackControl::resolveTimelineVerb(gain.ptr(), gain.size(), seen.resolver()), gain));
    // No arguments at all.
    const auto bare = osc_test::message(CLOCKWORK_SYS("track/timeline"));
    CHECK(same(TrackControl::resolveTimelineVerb(bare.ptr(), bare.size(), seen.resolver()), bare));
    // Not even OSC.
    const uint8_t junk[] = {1, 2, 3};
    const auto out = TrackControl::resolveTimelineVerb(junk, sizeof junk, seen.resolver());
    CHECK(out == std::vector<uint8_t>(junk, junk + sizeof junk));
    CHECK(seen.names.empty());
}
