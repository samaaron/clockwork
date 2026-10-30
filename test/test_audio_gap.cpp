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
 * steps at that block. Nothing that waited is dropped for a gap: it plays,
 * late by the gap. One line says the gap, and nothing else does. A huge
 * device buffer renders a block every 170 ms by
 * design and is not a gap: measuring against the audio rendered is what
 * keeps it from being one.
 */
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "clock/ClockworkClock.h"
#include "lanes/lanes.h"
#include "clockwork_client.h"
#include "metrics_schema.h"

#include <chrono>
#include <cmath>
#include <thread>

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

// The lines saying `text` from here on. The engine records its last block
// from boot, so a runner stalled between the fixture's boot and a test's own
// start makes that start a gap too, and says so: a test counts what its own
// blocks say, from just after its start.
struct LinesSince {
    LinesSince(EngineFixture& fix, const char* text) : fix(fix), text(text), at(linesSaying(fix, text)) {}
    int count() const { return linesSaying(fix, text) - at; }
    EngineFixture& fix;
    const char* text;
    int at;
};

}  // namespace

TEST_CASE("audio gap: a block every 170 ms on a huge device buffer is not a gap", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    const LinesSince resumed(fix, "audio resumed");
    // 8192 frames at 48 kHz: a block of 170.7 ms, every 170.7 ms, the first
    // 170.7 ms after the start (the device's own first delivery: never a gap).
    for (int i = 1; i <= 20; ++i)
        REQUIRE_FALSE(clockwork_audio_back(wall0 + i * 0.1707, mono0 + i * 0.1707, (i - 1) * 0.1707));
    CHECK(resumed.count() == 0);
}

TEST_CASE("audio gap: time passed with no audio rendered, over kClockStepSeconds, is a gap; "
          "a wall clock set forward is not, by the monotonic clock", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    const LinesSince resumed(fix, "audio resumed");
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.05, mono0 + 0.05, 0.05));
    // 0.55 s passed, 0.05 s of audio rendered in it.
    REQUIRE(clockwork_audio_back(wall0 + 0.6, mono0 + 0.6, 0.1));
    // The wall set forward by an hour, the monotonic clock and the audio on as before.
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 3600.7, mono0 + 0.7, 0.2));
    // And set back: no more a gap.
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.8, mono0 + 0.8, 0.3));
    CHECK(resumed.count() == 1);
}

TEST_CASE("audio gap: a host with no monotonic clock is judged by its wall clock", "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, 0.0);
    const LinesSince resumed(fix, "audio resumed");
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.05, 0.0, 0.05));
    REQUIRE(clockwork_audio_back(wall0 + 0.6, 0.0, 0.1));
    CHECK(resumed.count() == 1);
}

TEST_CASE("audio gap: a host whose audio starts is judged by time alone, as nothing was rendered",
          "[gap][engine]") {
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    const LinesSince resumed(fix, "audio resumed");
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 0.05, mono0 + 0.05, 100.0));   // a count from any origin
    // Its audio starts again 3 s on (a device opened again): the gap is the 3 s, whatever its count says.
    REQUIRE(clockwork_audio_starts(wall0 + 3.05, mono0 + 3.05));
    // And on from there: its first block, then one on time.
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 3.1, mono0 + 3.1, 0.0));
    REQUIRE_FALSE(clockwork_audio_back(wall0 + 3.15, mono0 + 3.15, 0.05));
    CHECK(resumed.count() == 1);
}

TEST_CASE("audio gap: what waited through a gap plays when the audio is back, late by the gap, whatever "
          "the gap; one line says the gap and nothing else speaks", "[gap][engine]") {
    // The engine drops a message only for want of room, and counts it: a gap
    // of any length delivers what queued and fires what was scheduled, late.
    // A host that wants what waited gone says so itself (purge).
    EngineFixture fix(pumped());
    // The pump keeps the record too, a block at a time, from the wall it
    // started on: the gaps here are measured from that, not a made-up instant.
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    OscReply r;

    // A short gap: what was sent before it is answered after it.
    clockwork_audio_starts(wall0, mono0);
    const LinesSince resumed(fix, "audio resumed");
    REQUIRE_FALSE(clockwork_audio_back(wall0, mono0, 0.0));   // its first block
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(clockwork_audio_back(wall0 + 0.5, mono0 + 0.5, 0.0));
    REQUIRE(fix.waitForReply("/dummy/pong", r));
    CHECK(resumed.count() == 1);

    // A long gap: the same.
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(clockwork_audio_back(wall0 + 5.0, mono0 + 5.0, 0.0));
    REQUIRE(fix.waitForReply("/dummy/pong", r));
    CHECK(resumed.count() == 2);
    CHECK(linesSaying(fix, "[wake]") == 0);
    CHECK(linesSaying(fix, "stalled") == 0);
}

namespace {

// Tick one block with the given frame count and clocks; returns the block
// time the engine filled in.
ClockworkBlockTime tick(uint64_t frames, double wall, double mono) {
    ClockworkBlockTime t{};
    t.struct_bytes = sizeof t;
    t.audio_frames = frames;
    t.wall_ntp     = wall;
    t.monotonic_secs = mono;
    REQUIRE(clockwork_tick(&t, 2, 0));
    return t;
}
uint32_t anchors(EngineFixture& fix) {
    uint32_t words[clockwork::metrics_schema::kMetricWords] = {};
    REQUIRE(clockwork_client_metrics(fix.engine().egressClient(), words, clockwork::metrics_schema::kMetricWords) > 63);
    return words[63];   // clock_anchors
}
double publishedClock(EngineFixture& fix) {
    ClockworkClientSampleClock sc{};
    sc.struct_bytes = sizeof sc;
    REQUIRE(clockwork_client_sample_clock(fix.engine().egressClient(), &sc) == CLOCKWORK_OK);
    return sc.dac_ntp;
}

}  // namespace

TEST_CASE("the tick keeps the engine's clock, on every host: a block after a gap anchors it afresh there",
          "[gap][clock][tick][engine]") {
    // The host passes its frame count and clocks to the tick; the engine
    // anchors, slews and publishes its one clock. No host keeps a clock of
    // its own. Here the host is this thread, on a pumped engine with the
    // freewheel clock off so the clock follows the wall clock.
    auto cfg = pumped();
    cfg.freewheelClock = false;
    EngineFixture fix(cfg);
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();

    clockwork_audio_starts(wall0, mono0);                   // this host's audio starts: anchored at its first block
    const LinesSince resumed(fix, "audio resumed");
    tick(0, wall0, mono0);
    const uint32_t anchored = anchors(fix);
    tick(2400, wall0 + 0.05, mono0 + 0.05);                 // on time: no new anchor
    CHECK(anchors(fix) == anchored);
    CHECK(publishedClock(fix) == Catch::Approx(wall0 + 0.05).margin(0.01));

    // 0.75 s passed, 0.15 s of audio rendered: the clock re-anchors at this block, on the wall clock.
    const ClockworkBlockTime t = tick(9600, wall0 + 0.8, mono0 + 0.8);
    CHECK(anchors(fix) == anchored + 1);
    CHECK(t.block_ntp == Catch::Approx(wall0 + 0.8).margin(1e-5));
    CHECK(publishedClock(fix) == Catch::Approx(t.block_ntp).margin(1e-9));
    CHECK(resumed.count() == 1);
}

TEST_CASE("the tick on a freewheeling clock sees no gap, having no wall clock", "[gap][clock][tick][engine]") {
    // Deterministic rendering (freewheelClock): a driver preempted on a busy
    // machine, or a host pumping blocks whenever it likes, must not move the
    // timeline streams are placed on. The gap rule needs a wall clock, and a
    // freewheeling clock has none.
    EngineFixture fix(pumped());   // freewheeling
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    const LinesSince resumed(fix, "audio resumed");
    tick(0, wall0, mono0);
    const uint32_t anchored = anchors(fix);
    const ClockworkBlockTime t = tick(128, wall0 + 0.7, mono0 + 0.7);   // 0.7 s late with one block rendered
    CHECK(anchors(fix) == anchored);
    CHECK(resumed.count() == 0);
    REQUIRE(t.block_ntp > 0.0);
}

#if CLOCKWORK_LINK
TEST_CASE("the tick stamps a block in Link's clock from its samples exactly on a freewheeling clock",
          "[gap][clock][tick][link][engine]") {
    EngineFixture fix(pumped());   // freewheeling
    const double wall0 = wallClockNTP();
    const double mono0 = clockwork::monotonicSeconds();
    clockwork_audio_starts(wall0, mono0);
    const ClockworkBlockTime b0 = tick(0, wall0, mono0);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));   // late
    const ClockworkBlockTime b1 = tick(128, wallClockNTP(), clockwork::monotonicSeconds());
    const ClockworkBlockTime b2 = tick(128 + 48000, wallClockNTP(), clockwork::monotonicSeconds());   // early
    const int64_t blockMicros = std::llround(128.0 / 48000.0 * 1e6);
    CHECK(std::llabs(int64_t(b1.block_host_micros) - int64_t(b0.block_host_micros) - blockMicros) <= 1);
    CHECK(std::llabs(int64_t(b2.block_host_micros) - int64_t(b1.block_host_micros) - 1000000) <= 1);
}
#endif

TEST_CASE("a cold swap never moves the engine's clock back",
          "[gap][clock][engine][ColdSwap]") {
    // The device delivers two seconds of audio in the case's virtual time,
    // in an instant of real time, so the engine's clock stands two seconds
    // ahead of the wall clock. The user then reopens the device: a cold
    // swap, a new instance over a new arena. Its clock continues from where
    // the old one's stood. A new instance that anchored at the wall clock
    // would hand every reader a time two seconds before the last one they
    // read.
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    sys->useVirtualTime();
    sys->advanceBy(2'000'000);
    sys->useWallTime();
    const double before = publishedClock(fix);
    REQUIRE(before > 0.0);

    OscReply reply;
    fix.send(osc_test::message(CLOCKWORK_SYS("devices/reopen")));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), reply, 15000));
    REQUIRE(fix.waitForBlocks(8));
    const double after = publishedClock(fix);
    INFO("clock before the swap " << before << ", after " << after);
    CHECK(after >= before);
}
