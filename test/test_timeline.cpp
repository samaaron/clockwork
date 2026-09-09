// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_timeline.cpp — every beat<->time question is answered from a snapshot.
 *
 * ClockworkClock::timeline(id) copies one beat grid out of its source — the
 * ClockworkClockState region for Link (id 0), the follower registry for a midi
 * slot, a 60 BPM placeholder for a name nothing has claimed — as a
 * clockwork::Timeline in NTP seconds. The /clockwork/clock rpc verbs answer from that
 * copy and nothing else, so what a client is told is exactly what
 * timeline(id) says, in one domain, with no per-call clock conversion in
 * between. (They used to answer through Ableton Link's per-boot microsecond
 * clock, converting NTP in and out around every call, in two code paths that
 * had to agree.)
 *
 * The registry itself is tested in Rust (rust/clockwork-clock); this pins the C++
 * boundary over it and the wire behaviour of the verbs.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "OscTestUtils.h"
#include "clock/EngineClock.h"
#include "clock/ClockworkClock.h"
#include "clock/Timeline.h"
#include "clock/clock_math.h"
#include "shared_memory.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

// Everything a /clockwork/clock verb replied, parsed.
struct Replies {
    std::vector<osc_test::ParsedReply> all;
    const osc_test::ParsedReply& only() const {
        REQUIRE(all.size() == 1);
        return all.front();
    }
};

Replies ask(ClockworkClock& clock, const osc_test::Packet& req) {
    Replies out;
    const bool handled = handleClockCoreOsc(
        clock, req.ptr(), req.size(),
        [&](const uint8_t* d, uint32_t n) { out.all.push_back(osc_test::parseReply(d, n)); });
    REQUIRE(handled);
    return out;
}

osc_test::Packet timeQuery(const char* addr, int64_t ntpMicros, float quantum) {
    osc_test::Builder b;
    b.begin(addr) << static_cast<osc::int64>(ntpMicros) << quantum;
    return b.end();
}

osc_test::Packet beatQuery(const char* addr, double beat, float quantum) {
    osc_test::Builder b;
    b.begin(addr) << static_cast<osc::int64>(std::llround(beat * clockwork::kMicrobeatsPerBeat))
                  << quantum;
    return b.end();
}

osc_test::Packet floatMsg(const char* addr, float v) {
    osc_test::Builder b;
    b.begin(addr) << v;
    return b.end();
}

} // namespace

// ── The snapshot ─────────────────────────────────────────────────────────────

TEST_CASE("timeline: the Link timeline is the clock state, in NTP seconds", "[clock][timeline]") {
    ClockworkClock clock;
    clock.state()->setTempo(120.0, 1000.0);
    clock.state()->setTransport(true, 1000.5);

    const clockwork::Timeline t = clock.timeline(0);
    CHECK(t.id == 0);
    CHECK(t.bpm == 120.0);
    CHECK(t.anchor_beat == 0.0);
    CHECK(t.anchor_ntp == 1000.0);
    CHECK(t.playing == 1);
    CHECK(t.anchored == 1);          // Link's grid always exists
    CHECK(t.transition_ntp == 1000.5);
    CHECK(t.meter_num == 4);
    CHECK(t.meter_den == 4);
    CHECK(t.beatAt(1001.0) == 2.0);
    CHECK(t.timeAtBeat(4.0) == 1002.0);
    CHECK(t.phaseAt(1002.75, 4.0) == 1.5);
    CHECK(t.beatsPerBar() == 4.0);
    CHECK(t.barAt(1002.75) == 1.0);          // beat 5.5
    CHECK(t.beatInBarAt(1002.75) == 1.5);

    // The meter is the clock state's: set through the clock, read in the
    // snapshot, and it moves the bars and nothing else.
    REQUIRE(clock.setMeter(7, 8));
    const clockwork::Timeline m = clock.timeline(0);
    CHECK(m.meter_num == 7);
    CHECK(m.meter_den == 8);
    CHECK(m.beatsPerBar() == 3.5);
    CHECK(m.beatAt(1001.0) == 2.0);
    CHECK(m.barAt(1010.0) == 5.0);           // beat 20: bars at 0, 3.5, .. 17.5
    CHECK(m.beatInBarAt(1010.0) == 2.5);
    CHECK_FALSE(clock.setMeter(4, 3));       // refused, and the meter stands
    CHECK_FALSE(clock.setMeter(0, 4));
    CHECK(clock.timeline(0).meter_num == 7);

    // ...and it agrees with the per-field getters it replaces.
    CHECK(t.beatAt(1234.5) == clock.beatAtTime(1234.5, 4.0));
    CHECK(t.timeAtBeat(77.0) == clock.timeAtBeat(77.0, 4.0));
}

TEST_CASE("timeline: a never-claimed name is the 60 BPM placeholder", "[clock][timeline]") {
    ClockworkClock clock;
    const clockwork::Timeline t = clock.timeline(-1);
    CHECK(t.id == -1);
    CHECK(t.bpm == 60.0);
    CHECK(t.anchored == 0);
    CHECK(t.beatAt(90.0) == 90.0);
    CHECK(clock.timeline(7).id == -1);   // an unheld slot is the same answer
}

TEST_CASE("timeline: a midi slot is the registry's snapshot", "[clock][timeline]") {
    ClockworkClock clock;
    const int id = clock.claimMidiTimeline("iac-1", "IAC Driver Bus 1");
    REQUIRE(id == 1);
    clock.setMidiTimelineTempo(id, 100.0);

    const clockwork::Timeline t = clock.timeline(id);
    CHECK(t.id == id);
    CHECK_THAT(t.bpm, WithinAbs(100.0, 1e-9));
    CHECK(t.anchored == 0);              // no Start yet
    CHECK(t.playing == 0);
    CHECK_THAT(t.beatAt(t.anchor_ntp + 3.0), WithinAbs(t.anchor_beat + 5.0, 1e-9));

    // The name grammar routes to it, and the listing carries it.
    CHECK(clock.resolveTimeline("midi:iac-1") == id);
    CHECK(clock.resolveTimeline("midi") == id);
    CHECK(clock.resolveTimeline("link") == 0);
    CHECK(clock.resolveTimeline("midi:nope") == -1);
    const auto rows = clock.listTimelines();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "link");
    CHECK(rows[1].name == "midi:iac-1");
    CHECK(rows[1].raw == "IAC Driver Bus 1");
    CHECK(rows[1].primary);
    CHECK(rows[1].clocking);
    CHECK_THAT(rows[1].bpm, WithinAbs(100.0, 1e-9));

    clock.setMidiTimelineTransport(id, 0 /*Start*/, 0.0);
    CHECK(clock.timeline(id).playing == 1);
    CHECK(clock.timeline(id).anchored == 1);
    CHECK(clock.timelineIsPlaying(id));
    CHECK(clock.timelineIsAnchored(id));

    // Its meter is its own: the Link timeline's does not move.
    REQUIRE(clock.setMidiTimelineMeter(id, 3, 4));
    CHECK(clock.timeline(id).meter_num == 3);
    CHECK(clock.timeline(0).meter_num == 4);
    CHECK_FALSE(clock.setMidiTimelineMeter(id, 3, 5));
    CHECK_FALSE(clock.setMidiTimelineMeter(7, 3, 4));   // nothing holds 7

    clock.freeMidiTimeline(id);
    CHECK(clock.timeline(id).id == -1);
    CHECK(clock.listTimelines().size() == 1);
}

TEST_CASE("timeline: the follower registry changes fire the timelines callback",
          "[clock][timeline]") {
    ClockworkClock clock;
    int fired = 0;
    clock.setTimelinesChangedCallback([&] { ++fired; });
    const int id = clock.claimMidiTimeline("p", "P");
    CHECK(fired == 1);
    clock.claimMidiTimeline("p", "P");   // idempotent: nothing changed
    CHECK(fired == 1);
    clock.setMidiTimelineTempo(id, 90.0);
    CHECK(fired == 2);
    clock.freeMidiTimeline(id);
    CHECK(fired == 3);
}

// ── The verbs answer from the snapshot ───────────────────────────────────────

TEST_CASE("timeline: the rpc verbs answer from timeline(0)", "[clock][timeline][osc]") {
    ClockworkClock clock;
    clock.state()->setTempo(120.0, 3'000'000'000.0);
    const clockwork::Timeline t = clock.timeline(0);
    const int64_t at = 3'000'000'010'250'000;    // origin + 10.25 s, in NTP micros

    SECTION("beat_at_time") {
        const auto r = ask(clock, timeQuery("/clockwork/clock/rpc/beat_at_time", at, 4.0f)).only();
        CHECK(r.address == "/clockwork/clock/rpc/beat_at_time.reply");
        CHECK_THAT(r.argDouble(0), WithinAbs(t.beatAt(at * 1e-6), 1e-9));
        CHECK_THAT(r.argDouble(0), WithinAbs(20.5, 1e-9));
    }
    SECTION("phase_at_time") {
        const auto r = ask(clock, timeQuery("/clockwork/clock/rpc/phase_at_time", at, 4.0f)).only();
        CHECK_THAT(r.argDouble(0), WithinAbs(0.5, 1e-9));
    }
    SECTION("time_at_beat is the inverse, in NTP micros") {
        const auto r = ask(clock, beatQuery("/clockwork/clock/rpc/time_at_beat", 20.5, 4.0f)).only();
        CHECK(r.address == "/clockwork/clock/rpc/time_at_beat.reply");
        CHECK(r.argInt64(0) == at);
    }
    SECTION("beat_phase_at_time") {
        const auto r =
            ask(clock, timeQuery("/clockwork/clock/rpc/beat_phase_at_time", at, 4.0f)).only();
        CHECK_THAT(r.argDouble(0), WithinAbs(20.5, 1e-9));
        CHECK_THAT(r.argDouble(1), WithinAbs(0.5, 1e-9));
    }
    SECTION("beat_phase_now stamps a wall time and answers for it") {
        const auto r = ask(clock, floatMsg("/clockwork/clock/rpc/beat_phase_now", 4.0f)).only();
        const int64_t now = r.argInt64(0);
        CHECK(std::llabs(now - static_cast<int64_t>(std::llround(wallClockNTP() * 1e6))) < 1'000'000);
        CHECK_THAT(r.argDouble(1), WithinAbs(t.beatAt(now * 1e-6), 1e-6));
        CHECK_THAT(r.argDouble(2), WithinAbs(clockwork::wrapPhase(t.beatAt(now * 1e-6), 4.0), 1e-6));
    }
    SECTION("transport/time keeps its 0 sentinel until a transition exists") {
        auto r = ask(clock, osc_test::message("/clockwork/clock/transport/time/get")).only();
        CHECK(r.argInt64(0) == 0);
        clock.state()->setTransport(true, 3'000'000'005.5);
        r = ask(clock, osc_test::message("/clockwork/clock/transport/time/get")).only();
        CHECK(r.argInt64(0) == 3'000'000'005'500'000);
    }
    SECTION("time/now is NTP micros") {
        const auto r = ask(clock, osc_test::message("/clockwork/clock/time/now/get")).only();
        CHECK(std::llabs(r.argInt64(0) - static_cast<int64_t>(std::llround(wallClockNTP() * 1e6))) < 1'000'000);
    }
    SECTION("meter: 4/4 until set; a set answers nothing; the query echoes a token") {
        auto r = ask(clock, osc_test::message("/clockwork/clock/meter")).only();
        CHECK(r.address == "/clockwork/clock/meter.reply");
        CHECK(r.argInt(0) == 4);
        CHECK(r.argInt(1) == 4);
        CHECK(r.argCount() == 2);
        CHECK(ask(clock, osc_test::message("/clockwork/clock/meter", 7, 8)).all.empty());
        r = ask(clock, osc_test::message("/clockwork/clock/meter", 99)).only();
        CHECK(r.argInt(0) == 7);
        CHECK(r.argInt(1) == 8);
        CHECK(r.argInt(2) == 99);
        CHECK(r.argCount() == 3);
        // Refused meters leave it alone.
        ask(clock, osc_test::message("/clockwork/clock/meter", 4, 3));
        ask(clock, osc_test::message("/clockwork/clock/meter", 0, 4));
        r = ask(clock, osc_test::message("/clockwork/clock/meter")).only();
        CHECK(r.argInt(0) == 7);
        CHECK(r.argInt(1) == 8);
    }
    SECTION("bar answers for now, from the snapshot") {
        ask(clock, osc_test::message("/clockwork/clock/meter", 7, 8));
        const clockwork::Timeline m = clock.timeline(0);
        const auto r = ask(clock, osc_test::message("/clockwork/clock/bar", 5)).only();
        CHECK(r.address == "/clockwork/clock/bar.reply");
        const double now = wallClockNTP();
        // Within a second of asking the bar can only have moved by one.
        CHECK(std::fabs(r.argDouble(0) - m.barAt(now)) <= 1.0);
        CHECK(r.argDouble(1) >= 0.0);
        CHECK(r.argDouble(1) < 3.5);
        CHECK(r.argDouble(0) == std::floor(r.argDouble(0)));
        CHECK(r.argInt(2) == 7);
        CHECK(r.argInt(3) == 8);
        CHECK(r.argInt(4) == 5);
        CHECK(r.argCount() == 5);
    }
}

// ── Tokens ───────────────────────────────────────────────────────────────────

TEST_CASE("timeline: a trailing int32 is a correlation token and is echoed; nothing else is",
          "[clock][timeline][osc][token]") {
    ClockworkClock clock;
    clock.state()->setTempo(120.0, 3'000'000'000.0);

    // A verb with no args of its own: bare, then with a token.
    auto r = ask(clock, osc_test::message("/clockwork/clock/tempo/get")).only();
    CHECK(r.argCount() == 1);
    r = ask(clock, osc_test::message("/clockwork/clock/tempo/get", 42)).only();
    CHECK(r.argCount() == 2);
    CHECK_THAT(r.argDouble(0), WithinAbs(120.0, 1e-9));
    CHECK(r.argInt(1) == 42);
    // A trailing string is not a token.
    r = ask(clock, osc_test::message("/clockwork/clock/tempo/get", "not-a-token")).only();
    CHECK(r.argCount() == 1);

    // A verb with args of its own: the token comes AFTER them, and the
    // verb's own trailing float is never mistaken for one.
    const int64_t at = 3'000'000'010'000'000;
    r = ask(clock, timeQuery("/clockwork/clock/rpc/beat_at_time", at, 4.0f)).only();
    CHECK(r.argCount() == 1);
    CHECK_THAT(r.argDouble(0), WithinAbs(20.0, 1e-9));
    osc_test::Builder b;
    b.begin("/clockwork/clock/rpc/beat_at_time") << static_cast<osc::int64>(at) << 4.0f << 7;
    r = ask(clock, b.end()).only();
    CHECK(r.argCount() == 2);
    CHECK_THAT(r.argDouble(0), WithinAbs(20.0, 1e-9));
    CHECK(r.argInt(1) == 7);
    // An int64 trailing arg is the verb's, not a token.
    osc_test::Builder b2;
    b2.begin("/clockwork/clock/rpc/time_at_beat") << static_cast<osc::int64>(20'000'000) << 4.0f;
    r = ask(clock, b2.end()).only();
    CHECK(r.argCount() == 1);
    CHECK(r.argInt64(0) == at);

    // The refusal echoes it the same way.
    {
        std::vector<osc_test::ParsedReply> refusals;
        const auto req = osc_test::message("/clockwork/clock/no/such/verb", 99);
        replyClockUnsupported(req.ptr(), req.size(), [&](const uint8_t* d, uint32_t n) {
            refusals.push_back(osc_test::parseReply(d, n));
        });
        REQUIRE(refusals.size() == 1);
        CHECK(refusals[0].address == "/clockwork/clock/unsupported");
        CHECK(refusals[0].argString(0) == "/clockwork/clock/no/such/verb");
        CHECK(refusals[0].argInt(1) == 99);
    }
}

#if !CLOCKWORK_LINK
// ── The session-of-one ───────────────────────────────────────────────────────
// With no Link compiled in the session is the inline one in LinkSession.h,
// whose only clock is the state region. Its origin is placed when the clock
// enters service (bindStateToShm → anchorToWallClockIfUnset), and from then
// on every rpc answer is in the NTP domain — which is what a client that
// asks in NTP micros and sleeps until the reply is counting on.

TEST_CASE("timeline: the session-of-one answers in the NTP domain once bound",
          "[clock][timeline][osc][session-of-one]") {
    ClockworkClock clock;
    // Before it enters service the grid is unplaced: beat 0 at NTP 0, the
    // 1900 epoch. rpc/time_at_beat for beat 8 at 120 BPM is then four
    // seconds after 1900 — the defect the anchor exists to close.
    auto r = ask(clock, beatQuery("/clockwork/clock/rpc/time_at_beat", 8.0, 4.0f)).only();
    CHECK(r.argInt64(0) == 4'000'000);
    r = ask(clock, osc_test::message("/clockwork/clock/transport/time/get")).only();
    CHECK(r.argInt64(0) == 0);

    // Entering service: bound into a region, as the engine binds the arena.
    ClockworkClockState region;
    ClockworkClockState::initDefaults(region);
    const double before = wallClockNTP();
    clock.bindStateToShm(&region);
    const double after = wallClockNTP();
    const double origin = clock.getBeatOriginNtp();
    CHECK(origin >= before);
    CHECK(origin <= after);
    CHECK(clock.state() == &region);

    // time_at_beat: beat 8 is four seconds after the origin, in NTP micros.
    r = ask(clock, beatQuery("/clockwork/clock/rpc/time_at_beat", 8.0, 4.0f)).only();
    const int64_t t8 = r.argInt64(0);
    CHECK(std::llabs(t8 - static_cast<int64_t>(std::llround((origin + 4.0) * 1e6))) <= 1);
    // beat_at_time inverts it.
    r = ask(clock, timeQuery("/clockwork/clock/rpc/beat_at_time", t8, 4.0f)).only();
    CHECK_THAT(r.argDouble(0), WithinAbs(8.0, 1e-5));
    r = ask(clock, timeQuery("/clockwork/clock/rpc/beat_phase_at_time", t8, 4.0f)).only();
    CHECK_THAT(r.argDouble(0), WithinAbs(8.0, 1e-5));
    // On a bar line, from either side of it: t8 is rounded to the micro.
    const double phase = r.argDouble(1);
    CHECK(std::min(phase, 4.0 - phase) < 1e-5);
    // transport/time: the sentinel is gone; the transport's "since" is the
    // anchoring instant, stopped.
    r = ask(clock, osc_test::message("/clockwork/clock/transport/time/get")).only();
    CHECK(std::llabs(r.argInt64(0) - static_cast<int64_t>(std::llround(origin * 1e6))) <= 1);
    r = ask(clock, osc_test::message("/clockwork/clock/transport/get")).only();
    CHECK(r.argInt(0) == 0);
    CHECK(r.argInt(1) == 1);

    // The session's own mutators go through the same region: a tempo change
    // holds the beat, and a transport change stamps its time.
    ask(clock, floatMsg("/clockwork/clock/tempo/set", 60.0f));
    r = ask(clock, timeQuery("/clockwork/clock/rpc/beat_at_time", t8, 4.0f)).only();
    // The beat playing at the retempo instant (2 per second since the
    // origin, moments ago) was held, and from there t8 is under four
    // seconds away at one beat a second: a little over 4, never the 8 the
    // old origin would give.
    CHECK(r.argDouble(0) >= 4.0 - 1e-5);
    CHECK(r.argDouble(0) < 6.0);
    ask(clock, osc_test::message("/clockwork/clock/transport/set", 1));
    r = ask(clock, osc_test::message("/clockwork/clock/transport/time/get")).only();
    CHECK(r.argInt64(0) >= static_cast<int64_t>(std::llround(origin * 1e6)));
    CHECK(std::llabs(r.argInt64(0) - static_cast<int64_t>(std::llround(wallClockNTP() * 1e6))) < 1'000'000);

    // Bound twice is bound once: an origin that exists is not stamped over.
    const double placed = clock.getBeatOriginNtp();
    clock.bindStateToShm(&region);
    CHECK(clock.getBeatOriginNtp() == placed);
    clock.unbindStateFromShm();
    CHECK(clock.state() != &region);
}
#endif  // !CLOCKWORK_LINK

TEST_CASE("timeline: a midi timeline's verbs answer from its own snapshot",
          "[clock][timeline][osc]") {
    ClockworkClock clock;
    clock.state()->setTempo(120.0, 3'000'000'000.0);     // Link says 120
    // A manual set on a port nothing has clocked claims it at 90.
    ask(clock, floatMsg("/clockwork/clock/midi:usb-1/tempo/set", 90.0f));
    const int id = clock.resolveTimeline("midi:usb-1");
    REQUIRE(id > 0);
    const clockwork::Timeline t = clock.timeline(id);
    CHECK_THAT(t.bpm, WithinAbs(90.0, 1e-9));

    const int64_t at = static_cast<int64_t>(std::llround((t.anchor_ntp + 2.0) * 1e6));
    auto r = ask(clock, timeQuery("/clockwork/clock/midi:usb-1/rpc/beat_at_time", at, 4.0f)).only();
    CHECK(r.address == "/clockwork/clock/midi:usb-1/rpc/beat_at_time.reply");
    CHECK_THAT(r.argDouble(0), WithinAbs(t.anchor_beat + 3.0, 1e-6));   // 90 BPM: 3 beats in 2 s

    r = ask(clock, beatQuery("/clockwork/clock/midi:usb-1/rpc/time_at_beat", t.anchor_beat + 3.0, 4.0f)).only();
    CHECK(std::llabs(r.argInt64(0) - at) <= 1);

    // transport/get: not anchored until a Start.
    r = ask(clock, osc_test::message("/clockwork/clock/midi:usb-1/transport/get")).only();
    CHECK(r.argInt(0) == 0);
    CHECK(r.argInt(1) == 0);
    ask(clock, osc_test::message("/clockwork/clock/midi:usb-1/transport/set", 1));
    r = ask(clock, osc_test::message("/clockwork/clock/midi:usb-1/transport/get")).only();
    CHECK(r.argInt(0) == 1);
    CHECK(r.argInt(1) == 1);
    r = ask(clock, osc_test::message("/clockwork/clock/midi:usb-1/transport/time/get")).only();
    CHECK(std::llabs(r.argInt64(0) - static_cast<int64_t>(std::llround(wallClockNTP() * 1e6))) < 1'000'000);

    // The unclaimed-name placeholder answers too, at 60.
    r = ask(clock, osc_test::message("/clockwork/clock/midi:ghost/tempo/get")).only();
    CHECK_THAT(r.argDouble(0), WithinAbs(60.0, 1e-9));
    CHECK(clock.resolveTimeline("midi:ghost") == -1);      // a read never claims

    // The meter is per timeline, and setting one claims the port like
    // tempo/set does.
    CHECK(ask(clock, osc_test::message("/clockwork/clock/midi:usb-2/meter", 3, 4)).all.empty());
    const int id2 = clock.resolveTimeline("midi:usb-2");
    REQUIRE(id2 > 0);
    r = ask(clock, osc_test::message("/clockwork/clock/midi:usb-2/meter")).only();
    CHECK(r.address == "/clockwork/clock/midi:usb-2/meter.reply");
    CHECK(r.argInt(0) == 3);
    CHECK(r.argInt(1) == 4);
    r = ask(clock, osc_test::message("/clockwork/clock/midi:usb-1/meter")).only();
    CHECK(r.argInt(0) == 4);
    r = ask(clock, osc_test::message("/clockwork/clock/meter")).only();
    CHECK(r.argInt(0) == 4);
    r = ask(clock, osc_test::message("/clockwork/clock/midi:usb-2/bar", 7)).only();
    CHECK(r.address == "/clockwork/clock/midi:usb-2/bar.reply");
    CHECK(r.argInt(2) == 3);
    CHECK(r.argInt(3) == 4);
    CHECK(r.argInt(4) == 7);
    // A query on a name nothing holds reads the placeholder's 4/4 and claims nothing.
    r = ask(clock, osc_test::message("/clockwork/clock/midi:ghost/meter")).only();
    CHECK(r.argInt(0) == 4);
    CHECK(clock.resolveTimeline("midi:ghost") == -1);
}
