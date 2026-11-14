// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_host_transport.cpp — a host's own transport, taken by the timeline
 * (src/clock/host_transport.h): its tempo, its meter, playing or stopped, and
 * its beat, by request (the grid's bars on its bars) or by force (its beat
 * number). Nothing here sleeps: every instant is the test's.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "clock/host_transport.h"

#include <cmath>

using Catch::Matchers::WithinAbs;
using clockwork::hostBeat;
using clockwork::hostMeter;
using clockwork::hostPlaying;
using clockwork::hostTempo;

namespace {

constexpr double kOrigin = 1000.0;   // beat 0, NTP seconds
constexpr double kRate = 48000.0;    // frames a second: a frame is 1/24000 of a beat at 120 BPM

void anchor(ClockworkClockState& s, double bpm) {
    ClockworkClockState::initDefaults(s);
    s.setTempo(bpm, kOrigin);
}

// The same, with the transport playing already, so a host that plays moves only what a case is about.
void anchorPlaying(ClockworkClockState& s, double bpm) {
    anchor(s, bpm);
    s.setTransport(true, kOrigin);
}

double beatAt(const ClockworkClockState& s, double ntp) { return readClockworkClock(&s).beatAt(ntp); }

// The instant the grid's beat is `beat`, at 120 BPM from kOrigin.
double at120(double beat) { return kOrigin + beat * 0.5; }

uint32_t request(ClockworkClockState& s, double beat, double at) { return hostBeat(s, beat, CLOCKWORK_BEAT_REQUEST, at, kRate); }
uint32_t force(ClockworkClockState& s, double beat, double at) { return hostBeat(s, beat, CLOCKWORK_BEAT_FORCE, at, kRate); }

} // namespace

TEST_CASE("host transport: the host's tempo is the grid's, and the beat playing stays where it is", "[clock][host]") {
    ClockworkClockState s;
    anchor(s, 120.0);
    const double now = at120(8.25);
    CHECK(hostTempo(s, 90.0, now) == clockwork::kHostMovedTempo);
    CHECK(readClockworkClock(&s).bpm == 90.0);
    CHECK_THAT(beatAt(s, now), WithinAbs(8.25, 1e-6));
    // A second later, a beat and a half on at 90.
    CHECK_THAT(beatAt(s, now + 1.0), WithinAbs(9.75, 1e-6));

    // The same tempo again is no change: the grid's generation stands.
    const uint32_t generation = readClockworkClock(&s).generation;
    CHECK(hostTempo(s, 90.0, now + 1.0) == 0u);
    CHECK(readClockworkClock(&s).generation == generation);
}

TEST_CASE("host transport: a tempo that is no number is left out, and one under the clock's least is the least",
          "[clock][host]") {
    ClockworkClockState s;
    anchor(s, 120.0);
    for (double bad : { 0.0, -3.0, std::nan(""), HUGE_VAL }) {
        CHECK(hostTempo(s, bad, at120(1.0)) == 0u);
        CHECK(readClockworkClock(&s).bpm == 120.0);
    }
    // As a client's is (ClockworkClockState::retempo): the clock's own limit, and no other.
    ClockworkClockState c;
    anchor(c, 120.0);
    c.retempo(0.5, at120(1.0));
    CHECK(hostTempo(s, 0.5, at120(1.0)) == clockwork::kHostMovedTempo);
    CHECK(readClockworkClock(&s).bpm == readClockworkClock(&c).bpm);
    // Said again, it is the tempo there already.
    CHECK(hostTempo(s, 0.5, at120(2.0)) == 0u);
}

TEST_CASE("host transport: the host's meter is the grid's; one no bar can be built from is left out", "[clock][host]") {
    ClockworkClockState s;
    anchor(s, 120.0);
    CHECK(hostMeter(s, 7, 8) == clockwork::kHostMovedMeter);
    CHECK(readClockworkClock(&s).meter_num == 7);
    CHECK(readClockworkClock(&s).meter_den == 8);
    CHECK(hostMeter(s, 7, 8) == 0u);

    CHECK(hostMeter(s, 4, 3) == 0u);
    CHECK(readClockworkClock(&s).meter_num == 7);
}

TEST_CASE("host transport: playing and stopped are the host's, with when each began", "[clock][host]") {
    ClockworkClockState s;
    anchor(s, 120.0);
    CHECK(hostPlaying(s, true, at120(4.0)) == clockwork::kHostMovedTransport);
    CHECK(readClockworkClock(&s).is_playing);
    CHECK(readClockworkClock(&s).is_playing_at_ntp == at120(4.0));
    CHECK(hostPlaying(s, true, at120(5.0)) == 0u);
    CHECK(readClockworkClock(&s).is_playing_at_ntp == at120(4.0));

    CHECK(hostPlaying(s, false, at120(6.0)) == clockwork::kHostMovedTransport);
    CHECK_FALSE(readClockworkClock(&s).is_playing);
    CHECK(readClockworkClock(&s).is_playing_at_ntp == at120(6.0));
}

TEST_CASE("host transport: a beat requested moves the grid's bars onto the host's by the shorter way", "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    // The grid is a quarter into beat 2 of its bar (beat 10.25); the host is on beat 1 of its bar (33.0).
    const double now = at120(10.25);
    CHECK(request(s, 33.0, now) == clockwork::kHostMovedGrid);
    // Back by a beat and a quarter, not on by two and three quarters: the beat count stays near its own.
    CHECK_THAT(beatAt(s, now), WithinAbs(9.0, 1e-6));

    // The other way round the bar: the grid a quarter into beat 0, the host a quarter before its bar line.
    anchorPlaying(s, 120.0);
    const double then = at120(8.25);
    CHECK(request(s, 3.75, then) == clockwork::kHostMovedGrid);
    CHECK_THAT(beatAt(s, then), WithinAbs(7.75, 1e-6));
}

TEST_CASE("host transport: a host that requests from another bar line moves nothing", "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    const double now = at120(10.0);   // beat 2 of the grid's bar
    CHECK(request(s, 2.0, now) == 0u);
    const uint32_t generation = readClockworkClock(&s).generation;
    // The host loops back sixteen bars, or on a hundred: the same place in the bar.
    CHECK(request(s, 2.02 - 64.0, now + 0.01) == 0u);
    CHECK(request(s, 2.04 + 400.0, now + 0.02) == 0u);
    CHECK(readClockworkClock(&s).generation == generation);
    CHECK_THAT(beatAt(s, now), WithinAbs(10.0, 1e-9));
}

TEST_CASE("host transport: a grid within half a frame of the host's is left as it is", "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    const double now = at120(12.0);
    const double frame = 120.0 / 60.0 / kRate;   // in beats
    CHECK(request(s, 0.4 * frame, now) == 0u);
    CHECK_THAT(beatAt(s, now), WithinAbs(12.0, 1e-9));
    CHECK(request(s, 0.6 * frame, now) == clockwork::kHostMovedGrid);
    CHECK_THAT(beatAt(s, now), WithinAbs(12.0 + 0.6 * frame, 1e-9));
    // By force as well.
    CHECK(force(s, 12.0 + 0.9 * frame, now) == 0u);
    CHECK(force(s, 12.0 + 1.2 * frame, now) == clockwork::kHostMovedGrid);
}

TEST_CASE("host transport: a beat forced gives the timeline the host's beat number", "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    const double now = at120(12.0);
    CHECK(force(s, 33.5, now) == clockwork::kHostMovedGrid);
    CHECK_THAT(beatAt(s, now), WithinAbs(33.5, 1e-9));
    // A loop back to the start takes the timeline with it, where a request would have moved nothing.
    CHECK(force(s, 0.0, now + 0.5) == clockwork::kHostMovedGrid);
    CHECK_THAT(beatAt(s, now + 0.5), WithinAbs(0.0, 1e-9));
}

TEST_CASE("host transport: a way of saying a beat that is neither, or a beat that is no number, is left out",
          "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    const double now = at120(12.0);
    CHECK(hostBeat(s, 33.5, 2u, now, kRate) == 0u);
    CHECK(hostBeat(s, std::nan(""), CLOCKWORK_BEAT_FORCE, now, kRate) == 0u);
    CHECK_THAT(beatAt(s, now), WithinAbs(12.0, 1e-9));
}

TEST_CASE("host transport: a bar is the meter's length", "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    // 6/8: three quarter-note beats a bar. The grid on beat 9 (a bar line), the host a beat into its bar.
    const double now = at120(9.0);
    CHECK(hostMeter(s, 6, 8) == clockwork::kHostMovedMeter);
    CHECK(request(s, 100.0, now) == clockwork::kHostMovedGrid);   // 100 = 33 bars and 1
    CHECK_THAT(beatAt(s, now), WithinAbs(10.0, 1e-6));
}

TEST_CASE("host transport: the tempo changes and the bars line up at one instant", "[clock][host]") {
    ClockworkClockState s;
    anchorPlaying(s, 120.0);
    const double now = at120(8.5);
    CHECK(hostTempo(s, 60.0, now) == clockwork::kHostMovedTempo);
    CHECK(request(s, 16.0, now) == clockwork::kHostMovedGrid);
    CHECK(readClockworkClock(&s).bpm == 60.0);
    CHECK_THAT(beatAt(s, now), WithinAbs(8.0, 1e-6));
    CHECK_THAT(beatAt(s, now + 1.0), WithinAbs(9.0, 1e-6));
}

TEST_CASE("host transport: a beat said while the timeline is stopped moves nothing", "[clock][host]") {
    ClockworkClockState s;
    anchor(s, 120.0);
    const double now = at120(10.25);
    CHECK(request(s, 33.0, now) == 0u);
    CHECK(force(s, 33.0, now) == 0u);
    CHECK_THAT(beatAt(s, now), WithinAbs(10.25, 1e-9));
}

TEST_CASE("host transport: a grid nobody has placed is placed on the host's", "[clock][host]") {
    ClockworkClockState s;
    ClockworkClockState::initDefaults(s);   // origin 0: unanchored
    s.setTransport(true, 4000.0);
    const double now = 5000.0;
    CHECK(request(s, 33.5, now) == clockwork::kHostMovedGrid);
    CHECK_THAT(beatAt(s, now), WithinAbs(1.5, 1e-6));

    ClockworkClockState f;
    ClockworkClockState::initDefaults(f);
    f.setTransport(true, 4000.0);
    CHECK(force(f, 33.5, now) == clockwork::kHostMovedGrid);
    CHECK_THAT(beatAt(f, now), WithinAbs(33.5, 1e-6));
}
