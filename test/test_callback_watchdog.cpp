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
 * The watchdog samples ClockworkProcessor::processCount; when it freezes for
 * longer than the stall window (and no swap/reopen is in flight) it must
 * restart the audio source, after which queued commands drain and the
 * engine answers again.
 *
 * Nothing here reads a system clock. The watchdog measures time on the
 * case's clock (Config::watchdogClockMs) and the headless driver paces on
 * the case's pacer (Config::headlessPacer, FakePacer): a stall is the
 * driver's thread stuck while the clocks move, exactly as the case says,
 * whatever the machine is doing.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakePacer.h"
#include "clockwork_client.h"
#include "clockwork_prefix.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace {

constexpr int kPollMs = 50;

// The two clocks of a case: the watchdog's, in milliseconds, and the
// driver's, in nanoseconds. Moved together by the helpers below, so the
// watchdog's windows and the driver's blocks agree on how much time passed.
struct CaseClock {
    std::shared_ptr<std::atomic<int64_t>> ms = std::make_shared<std::atomic<int64_t>>(0);
    std::shared_ptr<FakePacer> pacer = std::make_shared<FakePacer>();
};

ClockworkEngine::Config watchdogConfig(const CaseClock& clock) {
    auto cfg = EngineFixture::driverConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;  // fast for tests; production default is much larger
    cfg.watchdogPollMs   = kPollMs;
    cfg.watchdogClockMs  = [ms = clock.ms] { return ms->load(); };
    cfg.headlessPacer    = clock.pacer;
    return cfg;
}

// A fixture whose engine booted on the free-running pacer; the case holds
// the time from here.
struct Watched {
    CaseClock clock;
    EngineFixture fix;
    explicit Watched(const std::function<void(ClockworkEngine::Config&)>& tweak = {})
        : fix(configured(clock, tweak)) {
        clock.pacer->hold();
        REQUIRE(clock.pacer->settle());
    }
    static ClockworkEngine::Config configured(const CaseClock& clock,
                                              const std::function<void(ClockworkEngine::Config&)>& tweak) {
        auto cfg = watchdogConfig(clock);
        if (tweak) tweak(cfg);
        return cfg;
    }
};

// One poll's worth of time on both clocks, the driver rendering what it owes.
void tick(Watched& w) {
    w.clock.pacer->advanceMs(kPollMs);
    REQUIRE(w.clock.pacer->settle());
    w.clock.ms->fetch_add(kPollMs);
    w.fix.engine().watchdogPoll();
}

// `forMs` of the case's clocks with the source healthy: the source delivers
// its blocks before each poll, so the watchdog sees ticks in every window.
void runHealthy(Watched& w, int forMs) {
    for (int t = 0; t < forMs; t += kPollMs) tick(w);
}

// The case's clocks run; what the watchdog sees is whatever the source does.
bool runUntil(Watched& w, int forMs, const std::function<bool()>& done) {
    for (int t = 0; t < forMs; t += kPollMs) {
        tick(w);
        if (done()) return true;
    }
    return done();
}

// A reply, with the clocks moving meanwhile: the engine answers only as
// blocks are rendered, and blocks are rendered only as the case's time
// passes. The one wait that must not move time is the deaf check.
bool reply(Watched& w, const char* addr, OscReply& r, int forMs) {
    for (int t = 0; t < forMs; t += kPollMs) {
        if (w.fix.waitForReply(addr, r, 20)) return true;
        tick(w);
    }
    return w.fix.waitForReply(addr, r, 20);
}

// The engine's clock as a client reads it (clockwork_client_sample_clock).
double dacNtp(EngineFixture& fix) {
    ClockworkClientSampleClock sc{};
    sc.struct_bytes = sizeof(sc);
    REQUIRE(clockwork_client_sample_clock(fix.engine().egressClient(), &sc) == CLOCKWORK_OK);
    return sc.dac_ntp;
}

} // namespace

TEST_CASE("Watchdog: silent during normal operation", "[Watchdog]") {
    Watched w;
    // Tick healthily across several stall windows — the watchdog must not fire.
    runHealthy(w, 2000);
    REQUIRE(w.fix.engine().watchdogRecoveryCount() == 0);

    OscReply r;
    w.fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(reply(w, "/dummy/pong", r, 2000));
}

TEST_CASE("Watchdog: restarts a stalled audio source; what waited through the stall is answered, late, and the engine answers again",
          "[Watchdog]") {
    // The audio was away, and what waited for the engine through it is
    // delivered when it is back: the engine drops nothing for a gap (a host
    // that wants what waited gone purges it: restart_grid.spec.mjs). A
    // bundle that fell due meanwhile fires late; a bundle for later plays at
    // its time. The engine then answers what is sent after, and its clock
    // never went back.
    Watched w;
    runHealthy(w, 500);   // the watchdog has seen the source live
    const double clockAtStall = dacNtp(w.fix);

    // Due 300 ms from now, which the stall will cover: in the schedule before it starts.
    OscReply r;
    w.fix.send(osc_test::bundle(static_cast<uint64_t>((clockAtStall + 0.3) * 4294967296.0),
                                { osc_test::message("/dummy/events/get") }));
    w.fix.send(osc_test::message(CLOCKWORK_SYS("sync"), 1));
    REQUIRE(reply(w, CLOCKWORK_SYS("synced"), r, 2000));

    // The driver's thread sticks: mActiveSource stays Headless — exactly the
    // wedge state (source believed live, no ticks).
    w.fix.expectAudioAway();   // the stall is the point
    w.clock.pacer->stall();

    // Deaf: the command lands in the IN ring with nothing draining it, and no
    // time has passed for the watchdog, so nothing can have recovered it.
    w.fix.send(osc_test::message("/dummy/ping"));
    CHECK_FALSE(w.fix.waitForReply("/dummy/pong", r, 200));
    CHECK(w.fix.engine().watchdogRecoveryCount() == 0);
    // For well after the stall: it waits through it, and is not stale.
    w.fix.send(osc_test::bundle(static_cast<uint64_t>((clockAtStall + 6.0) * 4294967296.0),
                                { osc_test::message("/dummy/events/get") }));
    // The first bundle's time passes while the audio is away: 2.3 s of the
    // wall, with the watchdog's clock held, so the stall is seen only now.
    w.clock.pacer->advanceMs(2300);
    const double clockInStall = dacNtp(w.fix);

    // The watchdog must notice the frozen processCount and restart the source…
    REQUIRE(runUntil(w, 5000, [&] {
        return w.fix.engine().watchdogRecoveryCount() >= 1;
    }));

    // …after which what waited is answered, late: the ping, and the bundle
    // whose time passed in the stall…
    REQUIRE(reply(w, "/dummy/pong", r, 2000));
    REQUIRE(reply(w, "/dummy/events", r, 2000));
    CHECK_FALSE(w.fix.waitForReply(CLOCKWORK_SYS("error"), r, 100));

    // …and what is sent now is answered.
    w.fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(reply(w, "/dummy/pong", r, 2000));
    REQUIRE(w.fix.engine().audioSource()
            == ClockworkEngine::AudioSource::Headless);

    // The clock stepped to the wall as the audio came back, never going
    // back, so the bundle for later is still ahead of it — and plays at its
    // time, once that time comes.
    const double clockAfter = dacNtp(w.fix);
    INFO("the clock: " << (clockInStall - clockAtStall) << " in the stall, " << (clockAfter - clockAtStall) << " after");
    CHECK(clockInStall >= clockAtStall);
    CHECK(clockAfter >= clockInStall);
    CHECK(clockAfter > clockAtStall + 2.0);
    CHECK(clockAfter < clockAtStall + 6.0);
    CHECK_FALSE(w.fix.waitForReply("/dummy/events", r, 100));   // not yet
    REQUIRE(runUntil(w, 6000, [&] { return w.fix.waitForReply("/dummy/events", r, 20); }));
}

TEST_CASE("Watchdog: holds fire while a device swap is in flight", "[Watchdog]") {
    Watched w;
    runHealthy(w, 500);

    // Simulate a swap in flight, then stall the source underneath it.
    auto gate = w.fix.engine().testHoldSwapGate();
    w.fix.expectAudioAway();   // the stall is the point
    w.clock.pacer->stall();

    // Stalled far past the window, but the gate is held — no recovery.
    runUntil(w, 800, [] { return false; });
    REQUIRE(w.fix.engine().watchdogRecoveryCount() == 0);

    // Swap "finishes" — recovery may now proceed.
    gate.unlock();
    REQUIRE(runUntil(w, 5000, [&] {
        return w.fix.engine().watchdogRecoveryCount() >= 1;
    }));

    OscReply r;
    w.fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(reply(w, "/dummy/pong", r, 2000));
}

TEST_CASE("Watchdog: with no clock given, it polls on a thread of its own",
          "[Watchdog][rt]") {
    // Every other case here owns the watchdog's clock; a product does not, and
    // gets the engine's thread polling on steady_clock. Only that it runs is
    // asserted: a loaded machine can only make it recover sooner. The driver
    // is still on the case's pacer: its thread sticks when the case says.
    auto pacer = std::make_shared<FakePacer>();
    auto cfg = EngineFixture::driverConfig();
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;
    cfg.watchdogPollMs   = 50;
    cfg.headlessPacer    = pacer;
    EngineFixture fix(cfg);
    pacer->hold();
    REQUIRE(pacer->settle());
    fix.expectAudioAway();   // the stall is the point
    pacer->stall();
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
