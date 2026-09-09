// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_clock_state.cpp — the ClockworkClockState write protocol, in one place.
 *
 * ClockworkClockState has coherence rules (origin before tempo, timestamp before
 * transport flag) that used to live as comments beside ~15 hand-written
 * store() sequences across three files, plus the tempo re-anchor rule
 * (a tempo change must not move the beat that is playing) copied into each
 * of them. These tests pin the rules to the struct's own mutators so a writer
 * that goes through them cannot get the ordering or the arithmetic wrong, and
 * nothing here sleeps: the beat is evaluated at a `now` the test chooses.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "clock/clock_math.h"
#include "shared_memory.h"

#include <cmath>

using Catch::Matchers::WithinAbs;

namespace {

// Anchor `s` so that beat 0 is at `origin` under `bpm`. (Atomics don't copy,
// so the state is the caller's.)
void anchor(ClockworkClockState& s, double origin, double bpm) {
    ClockworkClockState::initDefaults(s);
    s.setTempo(bpm, origin);
}

} // namespace

TEST_CASE("clock state: setTempo publishes origin and tempo together",
          "[clock][state]") {
    ClockworkClockState s;
    ClockworkClockState::initDefaults(s);
    s.setTempo(90.0, 1000.0);
    const auto snap = readClockworkClock(&s);
    CHECK(snap.bpm == 90.0);
    CHECK(snap.beat_origin_ntp == 1000.0);
    CHECK_THAT(snap.beatAt(1010.0), WithinAbs(15.0, 1e-9));
}

TEST_CASE("clock state: retempo keeps the current beat", "[clock][state][tempo]") {
    // 120 bpm, origin at t=1000: ten seconds later we are at beat 20.
    ClockworkClockState s; anchor(s, 1000.0, 120.0);
    const double now = 1010.0;
    REQUIRE_THAT(readClockworkClock(&s).beatAt(now), WithinAbs(20.0, 1e-9));

    SECTION("raising the tempo") {
        const double held = s.retempo(240.0, now);
        const auto snap = readClockworkClock(&s);
        CHECK_THAT(held, WithinAbs(20.0, 1e-9));
        CHECK(snap.bpm == 240.0);
        CHECK_THAT(snap.beatAt(now),       WithinAbs(20.0, 1e-9));
        CHECK_THAT(snap.beatAt(now + 1.0), WithinAbs(24.0, 1e-9));   // 4 beats/s now
    }
    SECTION("lowering the tempo") {
        s.retempo(60.0, now);
        const auto snap = readClockworkClock(&s);
        CHECK_THAT(snap.beatAt(now),       WithinAbs(20.0, 1e-9));
        CHECK_THAT(snap.beatAt(now + 1.0), WithinAbs(21.0, 1e-9));
    }
    SECTION("the same tempo is a no-op for the grid") {
        s.retempo(120.0, now);
        CHECK_THAT(readClockworkClock(&s).beat_origin_ntp, WithinAbs(1000.0, 1e-9));
    }
}

TEST_CASE("clock state: retempo on an unanchored clock only sets the tempo",
          "[clock][state][tempo]") {
    // origin == 0 means "nobody has placed the grid yet" (anchorToWallClockIfUnset
    // owns that); a tempo change must not invent one.
    ClockworkClockState s;
    ClockworkClockState::initDefaults(s);
    REQUIRE(readClockworkClock(&s).beat_origin_ntp == 0.0);
    s.retempo(90.0, 12345.0);
    const auto snap = readClockworkClock(&s);
    CHECK(snap.bpm == 90.0);
    CHECK(snap.beat_origin_ntp == 0.0);
}

TEST_CASE("clock state: retempo clamps the tempo to a sane minimum",
          "[clock][state][tempo]") {
    ClockworkClockState s; anchor(s, 1000.0, 120.0);
    s.retempo(0.0, 1010.0);
    CHECK(readClockworkClock(&s).bpm == 1.0);
    s.retempo(std::nan(""), 1010.0);
    CHECK(readClockworkClock(&s).bpm == 1.0);
    // and the beat survived the clamp (nothing went NaN/inf).
    CHECK(std::isfinite(readClockworkClock(&s).beat_origin_ntp));
    CHECK_THAT(readClockworkClock(&s).beatAt(1010.0), WithinAbs(20.0, 1e-9));
}

TEST_CASE("clock state: setOrigin places the grid without touching the tempo",
          "[clock][state]") {
    ClockworkClockState s; anchor(s, 1000.0, 120.0);
    // Put beat 8 at t = 1004 (i.e. originFor(8, 1004, 120) = 1000 again).
    s.setOrigin(clockwork::originFor(8.0, 1004.0, 120.0));
    const auto snap = readClockworkClock(&s);
    CHECK(snap.bpm == 120.0);
    CHECK_THAT(snap.beatAt(1004.0), WithinAbs(8.0, 1e-9));
}

TEST_CASE("clock state: setTransport stamps when the transport changed",
          "[clock][state][transport]") {
    ClockworkClockState s;
    ClockworkClockState::initDefaults(s);
    s.setTransport(true, 500.5);
    auto snap = readClockworkClock(&s);
    CHECK(snap.is_playing);
    CHECK(snap.is_playing_at_ntp == 500.5);
    s.setTransport(false, 600.25);
    snap = readClockworkClock(&s);
    CHECK_FALSE(snap.is_playing);
    CHECK(snap.is_playing_at_ntp == 600.25);
}

TEST_CASE("clock state: setFlag edits one bit and leaves its siblings",
          "[clock][state]") {
    ClockworkClockState s;
    ClockworkClockState::initDefaults(s);
    s.setFlag(SC_FLAG_LINK_ENABLED, true);
    s.setFlag(SC_FLAG_START_STOP_SYNC, true);
    CHECK(readClockworkClock(&s).flags == (SC_FLAG_LINK_ENABLED | SC_FLAG_START_STOP_SYNC));
    s.setFlag(SC_FLAG_LINK_ENABLED, false);
    CHECK(readClockworkClock(&s).flags == SC_FLAG_START_STOP_SYNC);
    s.setFlag(SC_FLAG_START_STOP_SYNC, false);
    CHECK(readClockworkClock(&s).flags == 0u);
}

TEST_CASE("clock state: the meter is 4/4 until set and travels as one word",
          "[clock][state][meter]") {
    ClockworkClockState s;
    ClockworkClockState::initDefaults(s);
    auto snap = readClockworkClock(&s);
    CHECK(snap.meter_num == 4);
    CHECK(snap.meter_den == 4);
    s.setMeter(7, 8);
    snap = readClockworkClock(&s);
    CHECK(snap.meter_num == 7);
    CHECK(snap.meter_den == 8);
    // One word, so a reader can never see the new numerator over the old
    // denominator.
    CHECK(s.meter.load() == ClockworkClockState::packMeter(7, 8));
    CHECK(ClockworkClockState::meterNum(ClockworkClockState::packMeter(13, 16)) == 13);
    CHECK(ClockworkClockState::meterDen(ClockworkClockState::packMeter(13, 16)) == 16);
    // The grid is not the meter's business.
    CHECK(snap.bpm == clockwork::kDefaultBpm);
    CHECK(snap.beat_origin_ntp == 0.0);
    // A null state reads as the default meter, like the default tempo.
    CHECK(readClockworkClock(nullptr).meter_num == 4);
    CHECK(readClockworkClock(nullptr).meter_den == 4);
}

TEST_CASE("clock state: copyFrom reproduces every field", "[clock][state]") {
    ClockworkClockState src; anchor(src, 1000.0, 133.0);
    src.setTransport(true, 1001.0);
    src.setFlag(SC_FLAG_LINK_AUDIO_PUBLISH, true);
    src.setMeter(5, 4);

    ClockworkClockState dst;
    ClockworkClockState::initDefaults(dst);
    dst.copyFrom(src);

    const auto a = readClockworkClock(&src), b = readClockworkClock(&dst);
    CHECK(a.bpm == b.bpm);
    CHECK(a.beat_origin_ntp == b.beat_origin_ntp);
    CHECK(a.is_playing == b.is_playing);
    CHECK(a.is_playing_at_ntp == b.is_playing_at_ntp);
    CHECK(a.flags == b.flags);
    CHECK(a.meter_num == b.meter_num);
    CHECK(a.meter_den == b.meter_den);
    CHECK(b.meter_num == 5);
}

TEST_CASE("clock math: retempoOrigin is the origin that holds the beat",
          "[clock][math]") {
    const double origin = 1000.0, now = 1010.0;
    const double held = clockwork::beatAt(now, origin, 120.0);
    const double o2 = clockwork::retempoOrigin(origin, 120.0, 240.0, now);
    CHECK_THAT(clockwork::beatAt(now, o2, 240.0), WithinAbs(held, 1e-9));
    // Unanchored or nonsensical old tempo: leave the origin alone.
    CHECK(clockwork::retempoOrigin(0.0, 120.0, 240.0, now) == 0.0);
    CHECK(clockwork::retempoOrigin(origin, 0.0, 240.0, now) == origin);
}
