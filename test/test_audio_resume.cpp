// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_audio_resume.cpp — the device callback across a pause and resume, as a
 * device swap drives it: paused, it writes exact silence over a sounding
 * guest; resumed, it sounds again, finite and at the level it had.
 *
 * The callback is driven by hand. Pause is honoured in the device callback
 * and nowhere else — the headless driver and the manual pump render without
 * consulting it — so a case that let either of those run would pause nothing
 * and pass whatever pause did. The manual pump starts no audio source, which
 * leaves this thread the only caller of the callback and of the render, and
 * the buffers below safe to read.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "ClockworkProcessor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// The callback emits silence for its first few callbacks after a start or a
// resume, to absorb page faults (ClockworkProcessor::process).
constexpr int kWarmupCallbacks = 4;

struct Driven {
    explicit Driven(EngineFixture& fx)
        : cb(fx.engine().processor()),
          left(static_cast<size_t>(cb.bufferLength())),
          right(static_cast<size_t>(cb.bufferLength())) {}

    // One device callback into this case's buffers, as Smoothie's worklet
    // makes it. No inputs, and no host time: the clock's own fallback, as a
    // backend that supplies none.
    void drive() {
        float* outs[2] = { left.data(), right.data() };
        cb.process(nullptr, 0, outs, 2, cb.bufferLength(), smoothie::BlockTime{});
    }
    void past_warmup() { for (int i = 0; i <= kWarmupCallbacks; ++i) drive(); }

    float peak() const {
        float p = 0.0f;
        for (float v : left)  p = std::max(p, std::fabs(v));
        for (float v : right) p = std::max(p, std::fabs(v));
        return p;
    }
    bool finite() const {
        return std::all_of(left.begin(),  left.end(),  [](float v) { return std::isfinite(v); })
            && std::all_of(right.begin(), right.end(), [](float v) { return std::isfinite(v); });
    }

    ClockworkProcessor& cb;
    std::vector<float> left, right;
};

ClockworkEngine::Config manualPump() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}

}  // namespace

TEST_CASE("audio resume: paused, the callback writes silence over a sounding guest; resumed, it sounds as before",
          "[resume][engine]") {
    EngineFixture fx(manualPump());
    Driven d(fx);

    // A tone, so that "paused" and "running" differ: over an empty graph both
    // render zeros and neither check below could fail.
    fx.send(osc_test::message("/dummy/tone"));
    d.past_warmup();
    const float before = d.peak();
    REQUIRE(before > 0.05f);

    d.cb.pause();
    CHECK(d.cb.isPaused());
    d.drive();
    CHECK(d.peak() == 0.0f);

    d.cb.resume();
    CHECK_FALSE(d.cb.isPaused());
    d.past_warmup();
    CHECK(d.finite());
    // The level it had, not a burst of catch-up and not silence.
    CHECK(d.peak() > before * 0.5f);
    CHECK(d.peak() < before * 2.0f);
}

TEST_CASE("audio resume: a resume clears the gap detector, so the pause is not read as a stall",
          "[resume][engine]") {
    EngineFixture fx(manualPump());
    auto& cb = fx.engine().processor();
    cb.armGapDetector();
    REQUIRE(cb.gapDetectorArmed());
    cb.pause();
    cb.resume();
    // Armed across the resume, the first callback after it would measure the
    // whole pause as one gap between callbacks and log a stall that never was.
    CHECK_FALSE(cb.gapDetectorArmed());
}
