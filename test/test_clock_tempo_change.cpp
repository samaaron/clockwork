// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_clock_tempo_change.cpp — a tempo change must not move the beat.
 *
 * beatAt(t) is (t - origin) * bpm / 60, so a new tempo written against the old
 * origin moves the current beat by the ratio of the two. Raising the tempo
 * jumps the beat forward and everything already scheduled falls into the past;
 * lowering it jumps the beat BACKWARDS, so a client waiting on the next beat
 * waits for the clock to climb back to a beat it had nearly reached.
 *
 * Reported from a live build on 2026-09-01: a live loop playing a kick and
 * sleeping one beat drifted when the tempo was raised and stopped dead when it
 * was lowered. Both are this.
 *
 * IT WAS IN BOTH BACKINGS, which is why these run in both configurations:
 *
 *   Link OFF — LinkSession.h's session-of-one wrote the tempo and never
 *              touched the origin at all.
 *   Link ON  — LinkSession.cpp mirrored the tempo SYNCHRONOUSLY against the old
 *              origin and left the re-anchor to Link's async tempo callback, so
 *              every local tempo change opened a window in which the mirror
 *              reported a jumped beat. That one reads as correct until you run
 *              it: the re-anchor does arrive, just not before a client polling
 *              for the next beat has already seen the jump.
 *
 * These live in Clockwork suite, not the product's. ClockworkClock and
 * LinkSession are clockwork's, so a clockwork project that never links
 * scsynth still inherits the behaviour — and would have inherited the bug with
 * no coverage, which is exactly what happened here: the only ClockworkClock tests
 * in the tree were in clockwork's suite.
 */
#include <catch2/catch_test_macros.hpp>

#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

// setBpm → getBpm is eventually consistent with Link on: commitAppSessionState
// updates client state synchronously and posts session-timing work to Link's
// io thread. The contract is "eventually equals", not "instantly equals".
bool eventuallyBpm(ClockworkClock& sc, double expected, double eps = 1e-9) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);   // wedge-only
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::abs(sc.getBpm() - expected) < eps) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// The beat the clock reports right now, and the instant it was asked for.
struct Read { double t, beat; };
Read readNow(ClockworkClock& sc) {
    const double t = wallClockNTP();
    return { t, sc.beatAtTime(t, 4.0) };
}

// Nothing jumped across a change from `fromBpm` to `toBpm` made between two
// reads: the beat advanced by what the time between them allows at one tempo
// or the other — measured, so a machine that pauses between the reads moves
// the allowance with it rather than overrunning a fixed one. Against the old
// origin the beat moves by the tempo ratio: beats, not a sliver.
void checkContinuous(Read before, Read after, double fromBpm, double toBpm) {
    const double span = after.t - before.t;
    const double moved = after.beat - before.beat;
    constexpr double kEps = 1e-3;   // beats: rounding between clock domains
    INFO("span " << span << " s, moved " << moved << " beats");
    CHECK(moved >= span * std::min(fromBpm, toBpm) / 60.0 - kEps);
    CHECK(moved <= span * std::max(fromBpm, toBpm) / 60.0 + kEps);
}

// Put the grid somewhere definite, then let real time carry us into it. A full
// second at 120bpm is two beats in — far enough that a beat number moved by a
// tempo ratio cannot hide inside the tolerances below.
void settleTwoBeatsIn(ClockworkClock& sc) {
    sc.setBpm(120.0);
    REQUIRE(eventuallyBpm(sc, 120.0));
    sc.requestBeatAtTime(0.0, wallClockNTP(), 4.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

} // namespace

TEST_CASE("clock: raising the tempo does not move the current beat",
          "[clock][tempo]") {
    ClockworkClock sc;
    settleTwoBeatsIn(sc);

    const Read before = readNow(sc);
    sc.setBpm(240.0);
    REQUIRE(eventuallyBpm(sc, 240.0));
    const Read after = readNow(sc);

    // The beat may only advance by the real time these two reads span.
    // Against the old origin it DOUBLED.
    checkContinuous(before, after, 120.0, 240.0);
}

TEST_CASE("clock: lowering the tempo does not move the current beat",
          "[clock][tempo]") {
    ClockworkClock sc;
    settleTwoBeatsIn(sc);

    const Read before = readNow(sc);
    sc.setBpm(60.0);
    REQUIRE(eventuallyBpm(sc, 60.0));
    const Read after = readNow(sc);

    // THE ONE THAT STOPS A LIVE LOOP. Halving the tempo halved the beat number,
    // so the clock ran backwards and every pending beat became unreachable
    // until it caught up again.
    checkContinuous(before, after, 120.0, 60.0);
}

TEST_CASE("clock: the next beat comes no later across a tempo change",
          "[clock][tempo]") {
    ClockworkClock sc;
    settleTwoBeatsIn(sc);

    // What a `sleep 1` does: ask for the time of the beat after this one.
    const Read asked = readNow(sc);
    const double target = std::floor(asked.beat) + 1.0;
    CHECK(sc.timeAtBeat(target, 4.0) > asked.t);

    sc.setBpm(60.0);
    REQUIRE(eventuallyBpm(sc, 60.0));
    const double changed = wallClockNTP();   // the change happened before this

    // Whether the beat is still ahead now depends on how long the machine took
    // to make the change (a long enough pause and it has really passed). What
    // the change itself must never do is push it away:
    const double whenAfter = sc.timeAtBeat(target, 4.0);

    // NO FURTHER AWAY THAN THE ONE BEAT IT REPRESENTS. This is the whole
    // symptom: with the origin left where it was, halving the tempo halves the
    // current beat, so the wait for the next beat silently becomes several
    // beats long. The loop has not deadlocked — it is sleeping far longer than
    // it was told to, which is what "just blocked" looks like from outside.
    // The beat is at most one beat (a second, at 60) past the change.
    CHECK(whenAfter - changed <= 60.0 / 60.0 + 1e-3);
}
