// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron

// Pure unit tests for clockwork::audio::LivenessMonitor — no engine, no CoreAudio,
// no real clock (times are plain integers in an arbitrary unit).

#include <catch2/catch_test_macros.hpp>
#include "AudioRecovery.h"

using clockwork::audio::LivenessMonitor;
using clockwork::audio::LivenessPhase;

// The core invariant: after a stall, a SINGLE resumed tick must NOT read as
// Live. Liveness requires ticks sustained across the confirm window — otherwise
// a device emitting one callback per reopen (the post-wake CoreAudio wedge)
// masquerades as recovered forever.
TEST_CASE("LivenessMonitor: a single twitch tick after a stall is not Live",
          "[AudioRecovery][Liveness]") {
    LivenessMonitor mon(/*stallWindow*/1000, /*confirmWindow*/500);

    // Boot: ticking normally -> Live immediately (no confirm needed at start).
    mon.observe(100, 0);
    REQUIRE(mon.phase(0) == LivenessPhase::Live);

    // Ticks freeze; past the stall window it must read Stalled.
    mon.observe(100, 1200);
    REQUIRE(mon.phase(1200) == LivenessPhase::Stalled);

    // One twitch tick, then frozen again — the reopen-produced single callback.
    mon.observe(101, 1300);
    CHECK(mon.phase(1300) != LivenessPhase::Live);   // Confirming, not Live
    CHECK(mon.phase(1300) == LivenessPhase::Confirming);

    mon.observe(101, 1400);                          // no further advance
    CHECK(mon.phase(1400) != LivenessPhase::Live);
}

// ── RateSkewMonitor ──────────────────────────────────────────────────────────
// Times in ms, rates in frames/ms (48 = 48kHz). Standard monitor: 1000ms
// windows, 300ms max observation gap, 5% tolerance, 2 consecutive bad windows.

using clockwork::audio::RateSkewMonitor;

namespace {
RateSkewMonitor standardMonitor() {
    return RateSkewMonitor(/*window*/1000, /*maxGap*/300,
                           /*tolerance*/0.05, /*badWindowsRequired*/2);
}
} // namespace

// One transient stall skews one window; the next healthy window must clear the
// streak so a lone "[gap] audio callback stalled" can never cold-swap.
TEST_CASE("RateSkewMonitor: a single bad window resets on the next good one",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    mon.observe(0, 48.0, 0);                 // anchor: windows align to 1000s
    // Window [0,1000]: a 200ms stall inside it (frames flat) => 20% slow => bad.
    uint64_t frames = 0;
    for (int64_t t = 100; t <= 1000; t += 100) {
        if (t <= 800) frames += 4800;        // stall for the last 200ms
        mon.observe(frames, 48.0, t);
    }
    CHECK_FALSE(mon.skewed());
    // Window [1000,2000]: healthy — the streak must reset, not accumulate.
    for (int64_t t = 1100; t <= 2000; t += 100) {
        frames += 4800;
        mon.observe(frames, 48.0, t);
    }
    CHECK_FALSE(mon.skewed());
    // Window [2000,3000]: a later lone bad window still isn't enough.
    for (int64_t t = 2100; t <= 3000; t += 100) {
        frames += 2400;                      // 0.5x
        mon.observe(frames, 48.0, t);
    }
    CHECK_FALSE(mon.skewed());
}

// A sampling pause (benign skip, swap in flight, machine asleep) exceeds
// maxGap: the window spanning it is discarded rather than read as slowness.
TEST_CASE("RateSkewMonitor: an observation gap discards the window",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 900; t += 100)
        mon.observe(static_cast<uint64_t>(48 * t), 48.0, t);
    // 5s sleep: frames frozen. Without the gap check this window would read
    // as ~0.15x and start a streak.
    mon.observe(48 * 900, 48.0, 5900);
    CHECK_FALSE(mon.skewed());
    // Healthy afterwards from the re-anchored window.
    for (int64_t t = 6000; t <= 8000; t += 100)
        mon.observe(static_cast<uint64_t>(48 * 900 + 48 * (t - 5900)), 48.0, t);
    CHECK_FALSE(mon.skewed());
}

// A nominal-rate change (cold swap to a new rate) re-anchors: frames delivered
// against the old rate must not be judged against the new one.
TEST_CASE("RateSkewMonitor: a nominal rate change re-anchors",
          "[AudioRecovery][RateSkew]") {
    auto mon = standardMonitor();
    for (int64_t t = 0; t <= 900; t += 100)
        mon.observe(static_cast<uint64_t>(48 * t), 48.0, t);
    // Rate changes mid-window (48k -> 44.1k device): discard, no verdict.
    for (int64_t t = 1000; t <= 3000; t += 100)
        mon.observe(static_cast<uint64_t>(48 * 900 + 44 * (t - 900)), 44.1, t);
    CHECK_FALSE(mon.skewed());
}

// ── RateSkewPolicy ───────────────────────────────────────────────────────────
// The property that would have caught the storm as a design flaw: against a
// PERSISTENT mismatch the old remedy was an action that reproduced the
// mismatch, without limit. sonic-pi#3565: a mixer clocked at 44.1k reporting
// 48k, 79 cold swaps in one session, each one stopping the client's jobs.

using clockwork::audio::RateSkewAction;
using clockwork::audio::RateSkewPolicy;

TEST_CASE("RateSkewPolicy: a transient skew recovers as before, and a healthy "
          "window restarts the count", "[AudioRecovery][RateSkew][Policy]") {
    RateSkewPolicy policy(3, 0.05);
    // Post-sleep free-run, one episode: recover, then the device runs clean.
    REQUIRE(policy.next(0.3) == RateSkewAction::Recover);
    policy.acted(0.3);
    policy.healthy();
    CHECK(policy.streak() == 0);
    // An hour later, the same again: still a plain recovery, not the second
    // step of a streak.
    REQUIRE(policy.next(0.3) == RateSkewAction::Recover);
    policy.acted(0.3);
    policy.healthy();
    REQUIRE(policy.next(0.3) == RateSkewAction::Recover);
    policy.acted(0.3);
    CHECK(policy.streak() == 1);
    CHECK_FALSE(policy.adopted());
}

TEST_CASE("RateSkewPolicy: a different ratio is a different fault",
          "[AudioRecovery][RateSkew][Policy]") {
    RateSkewPolicy policy(3, 0.05);
    policy.acted(0.919);
    policy.acted(0.919);
    CHECK(policy.streak() == 2);
    // Not the mixer's 0.919 any more: a 0.3x free-run restarts the count, so
    // two unrelated episodes cannot add up to "persistent".
    REQUIRE(policy.next(0.3) == RateSkewAction::Recover);
    policy.acted(0.3);
    CHECK(policy.streak() == 1);
    // Within tolerance of the last one IS the same fault.
    REQUIRE(policy.next(0.32) == RateSkewAction::Recover);
    policy.acted(0.32);
    CHECK(policy.streak() == 2);
}

