// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_audio_block_clock.cpp — one per-block clock sequence for every driver.
 *
 * The JUCE device callback, the headless thread and the engine's manual
 * pump each render clockwork's blocks. What they must do to the clock before a
 * block is the same: step the audio-thread NTP, publish the sample-clock
 * anchor, take the Link Audio stamp, mirror the clock into the dashboard.
 * These cases pin that sequence as one entry point, so the three drivers
 * cannot drift apart again (the headless ones used to skip the dashboard).
 */
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "clock/ClockworkClock.h"
#include "native/AudioBlockClock.h"
#include "native/LinkAudioHost.h"
#include "shared_memory.h"

TEST_CASE("audio block clock: begin steps the NTP and stamps the block",
          "[clock][block]") {
    ClockworkClock clock;
    LinkAudioHost host(clock);
    const double sr = 48000.0;
    clock.setFreewheelClock(true);        // NTP is the sample count, exactly
    clockwork::resetAudioBlockClock(clock, host, 0.0, sr);

    const clockwork::BlockTime b0 = clockwork::beginAudioBlock(clock, host, 0.0,    sr, 0, nullptr);
    const clockwork::BlockTime b1 = clockwork::beginAudioBlock(clock, host, 4800.0, sr, 0, nullptr);

    CHECK(b0.ntp > 0.0);                                  // anchored to the wall clock
    // 4800 samples; the margin is double resolution at NTP magnitude (~1e-6).
    CHECK(b1.ntp - b0.ntp == Catch::Approx(0.1).margin(1e-5));
    CHECK(b1.ntp == clock.now());                         // what the block sees
#if CLOCKWORK_LINK
    CHECK(b0.hostMicros > 0);
    CHECK(b1.hostMicros > b0.hostMicros);
#else
    CHECK(b0.hostMicros == 0);
    CHECK(b1.hostMicros == 0);
#endif
}

TEST_CASE("audio block clock: begin mirrors the clock into the dashboard, whoever drives",
          "[clock][block]") {
    ClockworkClock clock;
    LinkAudioHost host(clock);
    const double sr = 48000.0;
    clock.setFreewheelClock(true);
    clockwork::resetAudioBlockClock(clock, host, 0.0, sr);
    clock.setBpm(120.0);

    PerformanceMetrics m{};
    clockwork::beginAudioBlock(clock, host, 0.0, sr, 0, &m);
    CHECK(m.clock_tempo_mbpm.load() == 120000u);
    CHECK(m.clock_playing.load() == 0u);
    // The origin is 0 here, so the beat count is every beat since 1900 — far
    // past what a 32-bit centi-beat holds. It saturates rather than taking
    // whatever the platform's out-of-range cast returns (0 on x86).
    CHECK(m.clock_beat_centi.load() == UINT32_MAX);
    CHECK(m.clock_phase_centi.load() < 400u);

    // A driver with no dashboard (unit contexts, no segment) passes null.
    clockwork::beginAudioBlock(clock, host, 128.0, sr, 0, nullptr);
}

#if CLOCKWORK_LINK
#include "clock/LinkSession.h"

#include <chrono>
#include <thread>

namespace {
// Blocks of 128 at 48 kHz: 2666.67 microseconds each.
constexpr double kSr = 48000.0;
constexpr double kBlockMicros = 128.0 / kSr * 1e6;
}  // namespace

// The stamp's rule is BlockStamp's, tested on made-up times in
// test_block_stamp.cpp. These are that the host hands it Link's own clock and
// the clock's freewheel setting, and that a driver's reset reaches it.

TEST_CASE("audio block clock: the block after a reset is stamped when Link's clock says",
          "[clock][block][link]") {
    ClockworkClock clock;
    LinkAudioHost host(clock);
    clock.setFreewheelClock(true);
    clockwork::resetAudioBlockClock(clock, host, 0.0, kSr);
    clockwork::beginAudioBlock(clock, host, 0.0, kSr, 0, nullptr);
    clockwork::beginAudioBlock(clock, host, 10 * kSr, kSr, 0, nullptr);   // 10 s of samples on, at once
    clockwork::resetAudioBlockClock(clock, host, 10 * kSr, kSr);
    const int64_t before = clock.linkSession().linkClockMicrosRaw();
    const auto    block  = clockwork::beginAudioBlock(clock, host, 10 * kSr, kSr, 0, nullptr);
    const int64_t after  = clock.linkSession().linkClockMicrosRaw();
    CHECK(int64_t(block.hostMicros) >= before);
    CHECK(int64_t(block.hostMicros) <= after);
}

TEST_CASE("audio block clock: with a freewheel clock a block's stamp is its samples exactly",
          "[clock][block][link]") {
    // Deterministic rendering (ClockworkClock::setFreewheelClock): a driver
    // preempted on a busy machine, or a host pumping blocks whenever it likes,
    // must not move the timeline a stream is placed on.
    ClockworkClock clock;
    LinkAudioHost host(clock);
    clock.setFreewheelClock(true);
    clockwork::resetAudioBlockClock(clock, host, 0.0, kSr);
    const auto b0 = clockwork::beginAudioBlock(clock, host, 0.0, kSr, 0, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));   // late
    const auto b1 = clockwork::beginAudioBlock(clock, host, 128.0, kSr, 0, nullptr);
    const auto b2 = clockwork::beginAudioBlock(clock, host, 128.0 + 48000.0, kSr, 0, nullptr);   // early
    CHECK(std::llabs(int64_t(b1.hostMicros) - int64_t(b0.hostMicros) - std::llround(kBlockMicros)) <= 1);
    CHECK(std::llabs(int64_t(b2.hostMicros) - int64_t(b1.hostMicros) - 1000000) <= 1);
}
#endif
