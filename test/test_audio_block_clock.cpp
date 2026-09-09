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
