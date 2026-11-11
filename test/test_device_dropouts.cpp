// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_dropouts.cpp — what a dropout leaves behind when every block
 * rendered in time (ClockworkProcessor::noteDeviceTime).
 *
 * A device can lose audio that the engine rendered on time: its driver missed
 * a cycle, or its own sample clock skipped. And a callback can come with no
 * block to play. Each of these is counted since the engine started, in the
 * metrics block's words 66-71, and said in the log, so a dropout someone hears
 * is a dropout the engine can show. The stand-in device's driver says what a
 * real one does: its sample clock with each buffer, and its count of missed
 * cycles. And how the device's thread is scheduled is said as it starts, for
 * a dropout that was the scheduler's.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "core_state.h"   // GateHold: a control call holding the engine

#include <chrono>
#include <string>
#include <thread>

namespace {

int linesSaying(EngineFixture& fix, const std::string& text) {
    int n = 0;
    for (const auto& line : fix.debugMessages())
        if (line.find(text) != std::string::npos) ++n;
    return n;
}

}  // namespace

TEST_CASE("device dropouts: a steady device counts nothing", "[dropouts][engine]") {
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    sys->useVirtualTime();
    // advanceBy returns once the device has delivered every block due: each
    // callback has published what it counted.
    sys->advanceBy(500'000);
    const auto& m = fix.engine().getMetrics();
    REQUIRE(m.callback_gap_max_us.load() > 0u);   // the counts are published: 0 below is a count
    CHECK(m.device_missed_cycles.load() == 0u);
    CHECK(m.device_clock_jumps.load() == 0u);
    CHECK(m.device_clock_jump_frames.load() == 0u);
    CHECK(m.silent_callbacks.load() == 0u);
    fix.pollUntil([] { return false; }, 200);   // what the control pass would carry, carried
    CHECK(linesSaying(fix, "[device]") == 0);
}

TEST_CASE("device dropouts: the device's sample clock jumping is counted, with the frames it skipped, and said",
          "[dropouts][engine]") {
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    sys->useVirtualTime();
    sys->advanceBy(200'000);
    const auto& m = fix.engine().getMetrics();
    REQUIRE(m.device_clock_jumps.load() == 0u);

    // The device skips 480 frames before a buffer, as hardware that missed
    // a cycle does.
    sys->at("Fake Speakers").skipFrames = 480;
    sys->advanceBy(100'000);
    REQUIRE(fix.pollUntil([&] { return m.device_clock_jumps.load() == 1u; }));
    CHECK(m.device_clock_jump_frames.load() == 480u);
    REQUIRE(fix.pollUntil([&] { return linesSaying(fix, "the device's clock moved +480 frames") == 1; }));

    // And replays some: a jump, with no frames skipped.
    sys->at("Fake Speakers").skipFrames = -64;
    sys->advanceBy(100'000);
    REQUIRE(fix.pollUntil([&] { return m.device_clock_jumps.load() == 2u; }));
    CHECK(m.device_clock_jump_frames.load() == 480u);
}

TEST_CASE("device dropouts: the cycles the driver says the device missed are counted from when it started, and said",
          "[dropouts][engine]") {
    auto sys = fake_audio::makeSimpleSystem();
    // Misses the driver counted before the engine saw the device are not the
    // engine's to count.
    sys->at("Fake Speakers").missedCycles = 5;
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    sys->useVirtualTime();
    sys->advanceBy(200'000);
    const auto& m = fix.engine().getMetrics();
    REQUIRE(m.callback_gap_max_us.load() > 0u);   // the counts are published: 0 below is a count
    CHECK(m.device_missed_cycles.load() == 0u);

    sys->at("Fake Speakers").missedCycles = 8;
    sys->advanceBy(100'000);
    REQUIRE(fix.pollUntil([&] { return m.device_missed_cycles.load() == 3u; }));
    REQUIRE(fix.pollUntil([&] { return linesSaying(fix, "the driver says the device missed 3 cycles") == 1; }));

    sys->at("Fake Speakers").missedCycles = 9;
    sys->advanceBy(100'000);
    REQUIRE(fix.pollUntil([&] { return m.device_missed_cycles.load() == 4u; }));
}

TEST_CASE("device dropouts: a callback with no block to play is counted as silence, and says why",
          "[dropouts][engine]") {
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    sys->useVirtualTime();
    sys->advanceBy(200'000);
    const auto& m = fix.engine().getMetrics();
    REQUIRE(m.silent_callbacks.load() == 0u);
    {
        // A control call holds the engine while the device plays on.
        const clockwork::GateHold control(fix.engine().core(), CLOCKWORK_ENTRY_CONTROL,
                                          clockwork::GateHold::Take::Wait);
        sys->advanceBy(10'000);
    }
    sys->advanceBy(100'000);
    REQUIRE(fix.pollUntil([&] { return m.silent_callbacks.load() >= 1u; }));
    REQUIRE(fix.pollUntil([&] { return linesSaying(fix, "played as silence: a control call held the engine") == 1; }));
}

// How the device's thread was left, said once as each device run starts:
// whether a dropout was the scheduler's is read against it.
TEST_CASE("device dropouts: the audio thread's scheduling is said as the device starts", "[dropouts][engine]") {
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.waitForBlocks(20));
    fix.clearDebugMessages();
    // A buffer change starts the device again, on a thread that may be new.
    REQUIRE(fix.engine().switchDevice("", 0, 256).success);
    REQUIRE(fix.waitForBlocks(40));
    INFO(fix.debugMessagesDump());
    REQUIRE(fix.pollUntil([&] { return linesSaying(fix, "[device-setup] audio thread ") == 1; }));
#if defined(__APPLE__)
    // The stand-in device's thread is an ordinary one, and macOS's is left as the driver set it.
    CHECK(linesSaying(fix, "[device-setup] audio thread left as the driver set it: SCHED_OTHER, priority ") == 1);
#endif
    REQUIRE(fix.waitForBlocks(40));
    CHECK(linesSaying(fix, "[device-setup] audio thread ") == 1);   // once a device run, not once a callback
}

TEST_CASE("device dropouts: the longest time between callbacks is kept, lately and since the engine started",
          "[dropouts][engine]") {
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fix(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    sys->useVirtualTime();
    sys->advanceBy(200'000);
    // No callback for at least 60 ms of the machine's time.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sys->advanceBy(100'000);
    const auto& m = fix.engine().getMetrics();
    REQUIRE(fix.pollUntil([&] { return m.callback_gap_max_us.load() >= 60'000u; }));
    CHECK(m.callback_gap_recent_us.load() >= 60'000u);
}
