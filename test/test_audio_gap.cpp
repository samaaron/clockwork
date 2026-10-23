// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_audio_gap.cpp — the one rule for audio that was away, on every host
 * (clockwork_audio_back, lanes.h).
 *
 * The gap is the time that passed since the last block less the audio
 * rendered in it, measured once a block where the host hands its time over,
 * by its monotonic clock where it has one (a system clock set by hand is not
 * a gap) and by its wall clock otherwise. Over kClockStepSeconds the clock
 * steps at that block; over kAudioAwaySeconds what waited and is due is
 * dropped too, and the schedule cleared. One line says it either way, and
 * nothing else does. A huge device buffer renders a block every 170 ms by
 * design and is not a gap: measuring against the audio rendered is what
 * keeps it from being one.
 */
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "clock/ClockworkClock.h"
#include "lanes/lanes.h"
#include "native/AudioBlockClock.h"
#include "native/LinkAudioHost.h"

#include <string>

namespace {

ClockworkEngine::Config pumped() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}


// The lines the rule said, once the control pass has carried them to the
// debug stream (a few passes, and a moment for the engine's own thread).
int linesSaying(EngineFixture& fix, const char* text) {
    for (int i = 0; i < 20; ++i) fix.engine().controlPass();
    fix.pollUntil([] { return false; }, 100);
    int n = 0;
    for (const auto& line : fix.debugMessages())
        if (line.find(text) != std::string::npos) ++n;
    return n;
}

}  // namespace

TEST_CASE("audio gap: a block every 170 ms on a huge device buffer is not a gap", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    // 8192 frames at 48 kHz: a block of 170.7 ms, every 170.7 ms, the first
    // 170.7 ms after the start (the device's own first delivery: never a gap).
    for (int i = 1; i <= 20; ++i)
        REQUIRE_FALSE(clockwork_audio_back(wall0 + i * 0.1707, mono0 + i * 0.1707, (i - 1) * 0.1707));
    CHECK(linesSaying(fix, "audio resumed") == 0);
}

TEST_CASE("audio gap: time passed with no audio rendered, over kClockStepSeconds, is a gap; "
          "a wall clock set forward is not, by the monotonic clock", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.05, mono0 + 0.05, 0.05));
    // 0.55 s passed, 0.05 s of audio rendered in it.
    REQUIRE(clockwork_audio_back(wall0 + 0.6, mono0 + 0.6, 0.1));
    // The wall set forward by an hour, the monotonic clock and the audio on as before.
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 3600.7, mono0 + 0.7, 0.2));
    // And set back: no more a gap.
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.8, mono0 + 0.8, 0.3));
    CHECK(linesSaying(fix, "audio resumed") == 1);
    CHECK(linesSaying(fix, "message buffer cleared") == 0);
}

TEST_CASE("audio gap: a host with no monotonic clock is judged by its wall clock", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, 0.0);
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.05, 0.0, 0.05));
    REQUIRE(clockwork_audio_back(wall0 + 0.6, 0.0, 0.1));
    CHECK(linesSaying(fix, "audio resumed") == 1);
}

TEST_CASE("audio gap: a host whose audio starts is judged by time alone, as nothing was rendered",
          "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.05, mono0 + 0.05, 100.0));   // a count from any origin
    // Its audio starts again 3 s on (a device opened again): the gap is the 3 s, whatever its count says.
    clockwork_audio_starts(wall0 + 3.05, mono0 + 3.05);
    // And on from there: its first block, then one on time.
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 3.1, mono0 + 3.1, 0.0));
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 3.15, mono0 + 3.15, 0.05));
    CHECK(linesSaying(fix, "message buffer cleared") == 1);
}

TEST_CASE("audio gap: over kClockStepSeconds what waited still plays; over kAudioAwaySeconds what "
          "waited and is due is dropped; one line says which, and nothing else speaks", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    OscReply r;

    // A short gap: what was sent before it is answered after it.
    clockwork_audio_starts(wall0, mono0);
    REQUIRE_FALSE(clockwork_audio_back(wall0, mono0, 0.0));   // its first block
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(clockwork_audio_back(wall0 + 0.5, mono0 + 0.5, 0.0));
    REQUIRE(fix.waitForReply("/dummy/pong", r));
    CHECK(linesSaying(fix, "audio resumed") == 1);
    CHECK(linesSaying(fix, "message buffer cleared") == 0);

    // A long gap: what was sent before it and is due is dropped; what is
    // sent after is answered.
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(clockwork_audio_back(wall0 + 5.0, mono0 + 5.0, 0.0));
    CHECK_FALSE(fix.waitForReply("/dummy/pong", r, 300));
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r));
    CHECK(linesSaying(fix, "audio resumed") == 2);
    CHECK(linesSaying(fix, "message buffer cleared") == 1);
    CHECK(linesSaying(fix, "[wake]") == 0);
}

TEST_CASE("audio block clock: a block after a gap steps the clock there", "[gap][clock][block]") {
    EngineFixture fix(pumped());   // the rule keeps its record in the engine's memory
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    ClockworkClock clock;
    LinkAudioHost host(clock);
    const double sr = 48000.0;
    clockwork_audio_starts(wall0, mono0);
    clockwork::resetAudioBlockClock(clock, host, 0.0, sr, wall0);
    const uint32_t anchors = clock.clockAnchors();

    // The first block, and one on time: no step.
    clockwork::beginAudioBlock(clock, host, 0.0, sr, 0, nullptr, wall0, mono0);
    clockwork::beginAudioBlock(clock, host, 2400.0, sr, 0, nullptr, wall0 + 0.05, mono0 + 0.05);
    CHECK(clock.clockAnchors() == anchors);

    // 0.75 s passed, 0.15 s of audio rendered: the clock steps to the wall at this block.
    const clockwork::BlockTime b = clockwork::beginAudioBlock(clock, host, 9600.0, sr, 0, nullptr,
                                                              wall0 + 0.8, mono0 + 0.8);
    CHECK(clock.clockAnchors() == anchors + 1);
    CHECK(b.ntp == Catch::Approx(wall0 + 0.8).margin(1e-5));
    CHECK(linesSaying(fix, "audio resumed") == 1);
}
