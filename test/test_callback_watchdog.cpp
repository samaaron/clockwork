// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_callback_watchdog.cpp — Callback-starvation watchdog.
 *
 * Guards the deaf-server failure mode: the audio source's thread stops
 * delivering callbacks (spinning inside the OS layer or dead) while the
 * engine still believes the source is active. Synth commands are drained
 * only by process_audio on the audio thread, so a stalled source turns the
 * whole server deaf — commands pile up in the IN ring forever — while the
 * control socket stays superficially alive.
 *
 * The watchdog samples JuceAudioCallback::processCount; when it freezes for
 * longer than the stall window (and no swap/reopen is in flight) it must
 * restart the audio source, after which queued commands drain and the
 * engine answers again.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace {

// The watchdog on the case's clock (Config::watchdogClockMs): it measures
// time only as the case moves it, so a stall is exactly what the case makes
// one, whatever the machine is doing.
struct CaseClock {
    std::shared_ptr<std::atomic<int64_t>> ms = std::make_shared<std::atomic<int64_t>>(0);
};

constexpr int kPollMs = 50;

ClockworkEngine::Config watchdogConfig(const CaseClock& clock) {
    auto cfg = EngineFixture::defaultConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;  // fast for tests; production default is much larger
    cfg.watchdogPollMs   = kPollMs;
    cfg.watchdogClockMs  = [ms = clock.ms] { return ms->load(); };
    return cfg;
}

// `forMs` of the case's clock with the source healthy: before each poll the
// source has delivered a block, so the watchdog sees ticks in every window.
void runHealthy(EngineFixture& fix, CaseClock& clock, int forMs) {
    for (int t = 0; t < forMs; t += kPollMs) {
        REQUIRE(fix.waitForBlocks(1, 30000));
        clock.ms->fetch_add(kPollMs);
        fix.engine().watchdogPoll();
    }
}

// The case's clock runs with no help for the source: what the watchdog sees
// is whatever the source does.
bool runUntil(EngineFixture& fix, CaseClock& clock, int forMs, const std::function<bool()>& done) {
    for (int t = 0; t < forMs; t += kPollMs) {
        clock.ms->fetch_add(kPollMs);
        fix.engine().watchdogPoll();
        if (done()) return true;
    }
    return done();
}

} // namespace

TEST_CASE("Watchdog: silent during normal operation", "[Watchdog]") {
    CaseClock clock;
    EngineFixture fix(watchdogConfig(clock));

    // Tick healthily across several stall windows — the watchdog must not fire.
    runHealthy(fix, clock, 2000);
    REQUIRE(fix.engine().watchdogRecoveryCount() == 0);

    OscReply r;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r));
}

TEST_CASE("Watchdog: restarts a stalled audio source and the engine answers again",
          "[Watchdog]") {
    CaseClock clock;
    EngineFixture fix(watchdogConfig(clock));
    runHealthy(fix, clock, 500);   // the watchdog has seen the source live

    // Kill the driver thread behind the engine's back: mActiveSource stays
    // Headless — exactly the wedge state (source believed live, no ticks).
    fix.stopHeadlessDriver();

    // Deaf: the command lands in the IN ring with nothing draining it, and no
    // time has passed for the watchdog, so nothing can have recovered it.
    OscReply r;
    fix.send(osc_test::message("/dummy/ping"));
    CHECK_FALSE(fix.waitForReply("/dummy/pong", r, 200));
    CHECK(fix.engine().watchdogRecoveryCount() == 0);

    // The watchdog must notice the frozen processCount and restart the source…
    REQUIRE(runUntil(fix, clock, 5000, [&] {
        return fix.engine().watchdogRecoveryCount() >= 1;
    }));

    // …after which the queued command drains and the engine answers.
    REQUIRE(fix.waitForReply("/dummy/pong", r, 30000));
    REQUIRE(fix.engine().audioSource()
            == ClockworkEngine::AudioSource::Headless);
}

TEST_CASE("Watchdog: holds fire while a device swap is in flight", "[Watchdog]") {
    CaseClock clock;
    EngineFixture fix(watchdogConfig(clock));
    runHealthy(fix, clock, 500);

    // Simulate a swap in flight, then stall the source underneath it.
    auto gate = fix.engine().testHoldSwapGate();
    fix.stopHeadlessDriver();

    // Stalled far past the window, but the gate is held — no recovery.
    runUntil(fix, clock, 800, [] { return false; });
    REQUIRE(fix.engine().watchdogRecoveryCount() == 0);

    // Swap "finishes" — recovery may now proceed.
    gate.unlock();
    REQUIRE(runUntil(fix, clock, 5000, [&] {
        return fix.engine().watchdogRecoveryCount() >= 1;
    }));

    OscReply r;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r, 30000));
}

TEST_CASE("Watchdog: with no clock given, it polls on a thread of its own",
          "[Watchdog]") {
    // Every other case here owns the watchdog's clock; a product does not, and
    // gets the engine's thread polling on steady_clock. Only that it runs is
    // asserted: a loaded machine can only make it recover sooner.
    auto cfg = EngineFixture::defaultConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;
    cfg.watchdogPollMs   = 50;
    EngineFixture fix(cfg);
    fix.stopHeadlessDriver();
    REQUIRE(fix.pollUntil([&] { return fix.engine().watchdogRecoveryCount() >= 1; }, 30000));
}

TEST_CASE("SwapGate: bounded acquisition fails while held, succeeds after release",
          "[Watchdog][SwapGate]") {
    // The primitive setDeviceMode's system-default reinit uses to serialise
    // against in-flight swaps (the unguarded interleave was the wedge's root
    // cause; the JUCE-manager race itself needs real devices, so the gate is
    // what's testable headlessly).
    EngineFixture fix;  // watchdog off

    // The gate is recursive, so it must be held by ANOTHER thread to contend a
    // tryAcquireSwapGate from this one (same-thread re-entry is allowed — that's
    // what lets recovery hold it across reopenCurrentDevice). Hold it on a helper
    // thread for the duration of the "held" assertion.
    std::mutex m;
    std::condition_variable cv;
    bool holding = false, release = false;
    std::thread holder([&] {
        auto held = fix.engine().testHoldSwapGate();
        { std::lock_guard<std::mutex> g(m); holding = true; } cv.notify_all();
        std::unique_lock<std::mutex> g(m);
        cv.wait(g, [&] { return release; });
    });
    { std::unique_lock<std::mutex> g(m); cv.wait(g, [&] { return holding; }); }

    std::unique_lock<std::recursive_mutex> lk;
    REQUIRE_FALSE(fix.engine().tryAcquireSwapGate(lk, 3, 10));   // held by holder thread
    REQUIRE_FALSE(lk.owns_lock());

    { std::lock_guard<std::mutex> g(m); release = true; } cv.notify_all();
    holder.join();

    REQUIRE(fix.engine().tryAcquireSwapGate(lk, 3, 10));         // now free
    REQUIRE(lk.owns_lock());
}
