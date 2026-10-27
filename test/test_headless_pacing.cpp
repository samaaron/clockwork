// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_headless_pacing.cpp — the headless driver on a clock the case owns.
 *
 * The headless driver is the engine's audio clock when there is no device:
 * every block it renders moves time forward by a block, and everything
 * scheduled is scheduled against that. Its pacing runs on OS timers, which
 * wake late on a starved machine, a laptop on battery, a loaded CI runner.
 * On 2026-10-27 a MacBook at 11% woke the driver 85 ms late on every block;
 * the driver dropped the backlog each time and rendered one block per wake,
 * and the engine's clock fell to a tenth of real time until the machine was
 * plugged in.
 *
 * These cases hand the driver a FakePacer and say exactly when it wakes,
 * so the rule is pinned without a timer in sight: a late wake under the
 * clock's own gap threshold (kClockStepSeconds) renders everything owed and
 * time stays on the wall; a wake past it drops the backlog and the clock
 * steps, as it does for a sleep.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakePacer.h"
#include "HeadlessDriver.h"
#include "clock/clock_math.h"
#include "clockwork_client.h"

#include <cmath>
#include <memory>

namespace {

constexpr int64_t kBlockNs = 1'000'000'000LL * 128 / 48000;   // the fixture's block: 2.667 ms

struct Paced {
    std::shared_ptr<FakePacer> pacer = std::make_shared<FakePacer>();
    EngineFixture fix;

    // The engine boots on a free-running pacer; then the case takes the time.
    Paced() : fix(config(pacer)) {
        pacer->hold();
        REQUIRE(pacer->settle());
    }

    static ClockworkEngine::Config config(const std::shared_ptr<FakePacer>& p) {
        auto cfg = EngineFixture::driverConfig();
        cfg.headlessPacer = p;
        return cfg;
    }

    uint32_t blocks() { return fix.engine().processor().processCount.load(std::memory_order_acquire); }

    // The engine's clock as a client reads it, against the pacer's wall.
    double clockBehindWall() {
        ClockworkClientSampleClock sc{};
        sc.struct_bytes = sizeof(sc);
        REQUIRE(clockwork_client_sample_clock(fix.engine().egressClient(), &sc) == CLOCKWORK_OK);
        return pacer->wallNtp() - sc.dac_ntp;
    }
};

} // namespace

TEST_CASE("HeadlessDriver: wakes on time render one block a period", "[HeadlessDriver][pacing]") {
    Paced p;
    const uint32_t before = p.blocks();
    for (int i = 0; i < 375; ++i) {
        p.pacer->advance(kBlockNs);
        REQUIRE(p.pacer->settle());
    }
    CHECK(p.blocks() - before == 375);
    CHECK(std::fabs(p.clockBehindWall()) < 2.0 * kBlockNs / 1e9);
}

TEST_CASE("HeadlessDriver: wakes 85 ms late still render every block, and the clock stays on the wall",
          "[HeadlessDriver][pacing]") {
    // A starved machine: every wake lands 85 ms past its deadline. The
    // blocks owed are rendered as a burst and time is not lost. The old
    // rule dropped the backlog past eight blocks (21 ms) and rendered one
    // block per wake: a clock at a tenth of real time.
    Paced p;
    const uint32_t before = p.blocks();
    const int64_t lateNs = 85'000'000;
    for (int i = 0; i < 12; ++i) {
        p.pacer->advance(lateNs);
        REQUIRE(p.pacer->settle());
    }
    const uint32_t owed = static_cast<uint32_t>(12 * lateNs / kBlockNs);   // 382
    const uint32_t got = p.blocks() - before;
    INFO("rendered " << got << " blocks of the " << owed << " a second of late wakes owes");
    CHECK(got + 1 >= owed);
    CHECK(got <= owed + 1);
    CHECK(std::fabs(p.clockBehindWall()) < 2.0 * kBlockNs / 1e9);
}

TEST_CASE("HeadlessDriver: a wake two seconds late drops the backlog and the clock steps to the wall",
          "[HeadlessDriver][pacing]") {
    // A sleep, or a thread held for longer than the clock's gap threshold:
    // replaying two seconds of blocks would be two seconds of audio in a
    // burst, so they are dropped, and the clock steps as the gap rule says.
    Paced p;
    const uint32_t before = p.blocks();
    p.pacer->advanceMs(2000);
    REQUIRE(p.pacer->settle());
    CHECK(p.blocks() - before <= 2);
    CHECK(std::fabs(p.clockBehindWall()) < 0.05);
}

TEST_CASE("HeadlessDriver: the catch-up rule is the clock's gap threshold", "[HeadlessDriver][pacing]") {
    // Pure: lag under kClockStepSeconds keeps the deadline (the loop catches
    // up); lag past it re-anchors to now (the backlog is dropped).
    const int64_t step = static_cast<int64_t>(clockwork::kClockStepSeconds * 1e9);
    const int64_t next = 1'000'000'000;
    CHECK(HeadlessDriver::cappedNextWake(next, next + step / 2) == next);
    CHECK(HeadlessDriver::cappedNextWake(next, next + step - 1) == next);
    CHECK(HeadlessDriver::cappedNextWake(next, next + step + 1) == next + step + 1);
    CHECK(HeadlessDriver::cappedNextWake(next, next - kBlockNs) == next);   // early: not a lag at all
}
