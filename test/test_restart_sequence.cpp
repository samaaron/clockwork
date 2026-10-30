// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_sequence.cpp — one cold restart, whatever started it.
 *
 * docs/RESTARTS.md names a cold restart as one sequence: restarting, with
 * the generation closing; the teardown's refusals; setup, the new instance's
 * first word, and running as the swap ends, with the generation opened. Three things start one — a device switch the user
 * asked for, the watchdog's recovery of a device that stopped delivering,
 * and the watchdog's recovery from a device that delivers at a rate other
 * than the one it reports — and a client cannot tell which, nor should it:
 * the sequence is the same, said in the same order with the same numbers.
 * These cases pin that, so the sequence stays one piece of code however the
 * engine's device paths move around it.
 *
 * A restart may build more than once before it says running — a device that
 * fails is rolled back, and the instance is built again on the way back — so
 * the generation it opens is above the one it closed, not always by one; and
 * the next restart closes exactly the one the last opened.
 */
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "clockwork_prefix.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;
using fake_audio::runWatchdogFor;
using fake_audio::runWatchdogUntil;

namespace {

// The sequence as a client hears it: every statechange and setup, in order,
// with the generation each carries.
struct Said { std::string address, state; int32_t generation; };

std::vector<Said> sequenceHeard(EngineFixture& fix) {
    std::vector<Said> out;
    for (const auto& r : fix.allReplies()) {
        const auto p = r.parsed();
        if (r.address == CLOCKWORK_SYS("statechange")) {
            REQUIRE(p.argCount() == 3);
            out.push_back({ r.address, p.argString(0), p.argInt(2) });
        } else if (r.address == CLOCKWORK_SYS("setup")) {
            REQUIRE(p.argCount() == 3);
            out.push_back({ r.address, "", p.argInt(2) });
        }
    }
    return out;
}

// The cold restarts in what was heard, each restarting(g), setup(h),
// running(h) with h above g, each closing the generation the last opened,
// starting from `from`. How many there were.
int coldRestartsIn(const std::vector<Said>& heard, int32_t from) {
    std::vector<Said> cold;
    for (const auto& s : heard)
        if (s.address == CLOCKWORK_SYS("setup") || s.state == "restarting" || s.state == "running")
            cold.push_back(s);
    REQUIRE(cold.size() % 3 == 0);
    int32_t g = from;
    const int n = static_cast<int>(cold.size() / 3);
    for (int i = 0; i < n; ++i) {
        const auto& a = cold[3 * i];
        const auto& b = cold[3 * i + 1];
        const auto& c = cold[3 * i + 2];
        INFO("cold restart " << i << ", closing generation " << g);
        CHECK(a.state == "restarting");
        CHECK(a.generation == g);
        CHECK(b.address == CLOCKWORK_SYS("setup"));
        CHECK(b.generation > g);
        CHECK(c.state == "running");
        CHECK(c.generation == b.generation);
        g = b.generation;
    }
    return n;
}

bool heardRunning(EngineFixture& fix, int32_t above) {
    for (const auto& s : sequenceHeard(fix))
        if (s.state == "running" && s.generation > above) return true;
    return false;
}

// The sequence once it has settled, read on the watchdog's clock: its last
// word a running naming the generation open now, and nothing more said for a
// further stall window, so every restart in it is complete. A recovery
// the engine asked for itself can start after the one a case waited for
// (the stall watchdog firing before the restored device has ticked), and a
// sequence read mid-restart has a restarting with no setup and running
// behind it yet.
std::vector<Said> settledSequence(EngineFixture& fix, fake_audio::FakeSystem& sys) {
    REQUIRE(runWatchdogUntil(fix.engine(), sys, 50, 20000, [&] {
        const auto h = sequenceHeard(fix);
        return !h.empty() && h.back().state == "running"
            && h.back().generation == static_cast<int32_t>(fix.engine().generation());
    }));
    const size_t settled = sequenceHeard(fix).size();
    REQUIRE(runWatchdogFor(fix.engine(), sys, 50, 1500));
    const auto heard = sequenceHeard(fix);
    REQUIRE(heard.size() == settled);
    return heard;
}

} // namespace

TEST_CASE("restart sequence: a device switch the user asked for",
          "[restarts][sequence][ColdSwap]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);
    subscribe(fix);
    const int32_t gen = static_cast<int32_t>(fix.engine().generation());

    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);
    REQUIRE(fix.pollUntil([&] { return heardRunning(fix, gen); }, 5000));
    fix.pumpBlock(4);

    // One restart, one generation on.
    const auto heard = sequenceHeard(fix);
    CHECK(coldRestartsIn(heard, gen) == 1);
    CHECK(heard.back().generation == gen + 1);
    CHECK(fix.engine().generation() == static_cast<uint32_t>(gen + 1));
}

TEST_CASE("restart sequence: the watchdog's recovery of a device that stopped delivering",
          "[restarts][sequence][Watchdog]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.callbackWatchdog           = true;
    cfg.watchdogPollMs             = 50;
    cfg.watchdogStallMs            = 500;
    cfg.watchdogRecoveryCooldownMs = 200;
    cfg.watchdogRateWindowMs       = 0;   // not the subject
    cfg.watchdogClockMs            = [sys] { return sys->nowMs(); };
    cfg.hostDrivesControl          = true;
    EngineFixture fix(cfg);
    subscribe(fix);
    const int32_t gen = static_cast<int32_t>(fix.engine().generation());

    // The speakers stop ticking as the user reopens them: a cold restart
    // that ends on silence. Then the watchdog's recovery, once they tick
    // again: the same sequence, started by the engine itself.
    sys->at("Fake Speakers").failStart = true;
    OscReply reply;
    fix.send(osc_test::message(CLOCKWORK_SYS("devices/reopen")));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), reply, 15000));
    const int32_t afterTheUsers = static_cast<int32_t>(fix.engine().generation());
    sys->at("Fake Speakers").failStart = false;
    sys->useVirtualTime();
    REQUIRE(runWatchdogUntil(fix.engine(), *sys, 50, 20000, [&] {
        return fix.engine().watchdogRecoveryCount() >= 1 && heardRunning(fix, afterTheUsers);
    }));
    const auto heard = settledSequence(fix, *sys);
    sys->useWallTime();
    INFO(fix.debugMessagesDump());
    CHECK(coldRestartsIn(heard, gen) >= 2);
    CHECK(heard.back().generation == static_cast<int32_t>(fix.engine().generation()));
}

TEST_CASE("restart sequence: the watchdog's recovery from a device delivering at the wrong rate",
          "[restarts][sequence][RateSkew]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Speakers").deliveredRate = 44100.0;   // reports 48000, ticks at 44100
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.callbackWatchdog           = true;
    cfg.watchdogPollMs             = 50;
    cfg.watchdogStallMs            = 1000;
    cfg.watchdogRateWindowMs       = 300;
    cfg.watchdogRateTolerance      = 0.05;
    cfg.watchdogRateBadWindows     = 2;
    cfg.watchdogRateMaxRecoveries  = 1;   // one recovery, one sequence
    cfg.watchdogRecoveryCooldownMs = 200;
    cfg.watchdogClockMs            = [sys] { return sys->nowMs(); };
    cfg.hostDrivesControl          = true;
    EngineFixture fix(cfg);
    subscribe(fix);
    const int32_t gen = static_cast<int32_t>(fix.engine().generation());

    sys->useVirtualTime();
    const bool recovered = runWatchdogUntil(fix.engine(), *sys, 50, 15000, [&] {
        return fix.engine().rateSkewRecoveryCount() >= 1 && heardRunning(fix, gen);
    });
    INFO(fix.debugMessagesDump());
    INFO("rate skew recoveries " << fix.engine().rateSkewRecoveryCount()
         << ", watchdog recoveries " << fix.engine().watchdogRecoveryCount()
         << ", heard running above " << gen << ": " << heardRunning(fix, gen)
         << ", engine state " << static_cast<int>(fix.engine().engineState())
         << ", device phase " << static_cast<int>(fix.engine().devicePhase())
         << ", recovery in flight " << fix.engine().recoveryInFlight());
    REQUIRE(recovered);
    const auto heard = settledSequence(fix, *sys);
    sys->useWallTime();
    CHECK(coldRestartsIn(heard, gen) >= 1);
    CHECK(heard.back().generation == static_cast<int32_t>(fix.engine().generation()));
}
