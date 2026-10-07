// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_rate_skew_recovery.cpp — the rate-skew watchdog against a device that
 * keeps time at a rate other than the one it reports.
 *
 * sonic-pi#3565: a USB mixer reports 48000 Hz to the OS and delivers ~44,100
 * frames a second. The watchdog measured it correctly (0.919x) and recovered
 * with a cold swap that reopened the device at the same 48000 — the exact
 * condition that triggered it — every ten seconds, 79 times in one session,
 * each swap stopping the client's jobs. The remedy is RateSkewPolicy: at most
 * N recoveries, the last of which requests the measured rate, and no further
 * swaps if the device skews at that rate too. The engine keeps playing.
 *
 * The fake device lies the same way (FakeDeviceSpec::deliveredRate), and
 * the case keeps the clock: the device delivers exactly the frames due by it
 * and the watchdog measures against it (fake_audio::runWatchdogUntil), so
 * every window reads the device's true ratio whatever the machine is doing,
 * and a session that took the mixer ten minutes plays out exactly the same
 * way every time, in a moment.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <mutex>
#include <string>
#include <vector>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;
using fake_audio::runWatchdogFor;
using fake_audio::runWatchdogUntil;

namespace {

ClockworkEngine::Config lyingDeviceConfig(std::shared_ptr<fake_audio::FakeSystem> sys) {
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.callbackWatchdog           = true;
    cfg.watchdogPollMs             = 50;
    cfg.watchdogStallMs            = 1000;   // not the subject: keep liveness quiet
    cfg.watchdogRateWindowMs       = 300;
    cfg.watchdogRateTolerance      = 0.05;
    cfg.watchdogRateBadWindows     = 2;
    cfg.watchdogRateMaxRecoveries  = 3;
    cfg.watchdogRecoveryCooldownMs = 200;
    cfg.watchdogClockMs            = [sys] { return sys->nowMs(); };
    // Drain the egress on a host thread, as SuperSonic's host does. With the
    // engine's own gateway the control pass runs once per audio block, so a
    // line logged just before a cold swap — every watchdog line, the user's
    // notice — is still in the ring when the swap rebuilds the arena, and is
    // lost with it. The assertions below on what was logged need the lines.
    cfg.hostDrivesControl          = true;
    return cfg;
}

// Every swap the engine performs, in order, as the recovery reported it.
struct SwapLog {
    std::mutex mu;
    std::vector<SwapResult> swaps;
    size_t count() { std::lock_guard<std::mutex> lk(mu); return swaps.size(); }
    SwapResult last() { std::lock_guard<std::mutex> lk(mu); return swaps.back(); }
};

constexpr int kPollMs = 50;   // lyingDeviceConfig's watchdogPollMs

int sessionRate(EngineFixture& fix) {
    return static_cast<int>(fix.engine().currentDevice().activeSampleRate);
}

// Every line logged so far has reached the fixture: the reply comes back
// through the same ring, after them.
void flushLog(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r, 30000));
}

// Subscribed to the engine's broadcasts as a GUI is: a recovery says so with
// /clockwork/devices/reopen.done, a rebuild with /clockwork/setup.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

std::vector<osc_test::ParsedReply> said(EngineFixture& fix, const std::string& address) {
    std::vector<osc_test::ParsedReply> out;
    for (auto& r : fix.allReplies())
        if (r.address == address) out.push_back(r.parsed());
    return out;
}

} // namespace

TEST_CASE("RateSkew: a device that reports 48k and delivers 44.1k is recovered at "
          "most N times, ends at the measured rate, and keeps playing",
          "[RateSkew][Watchdog]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Speakers").deliveredRate = 44100.0;   // reports 48000, ticks at 44100
    EngineFixture fix(lyingDeviceConfig(sys));

    SwapLog log;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult& r) {
        if (event.rfind("swap:", 0) == 0 || event.find("recover") != std::string::npos) {
            std::lock_guard<std::mutex> lk(log.mu);
            log.swaps.push_back(r);
        }
    };

    REQUIRE(sessionRate(fix) == 48000);
    sys->useVirtualTime();

    // Three recoveries: two plain reopens, then the adopt. Each needs two bad
    // 300 ms windows plus the cooldown. The adopt lands the session on the
    // rate the device actually keeps time at — which it advertises, so the
    // request snaps to it exactly.
    REQUIRE(runWatchdogUntil(fix.engine(), *sys, kPollMs, 15000, [&] {
        return fix.engine().rateSkewRecoveryCount() >= 3 && sessionRate(fix) == 44100;
    }));

    // From here the device is honest against its new nominal rate: no further
    // recoveries. Several verdict periods prove the storm is over.
    REQUIRE(runWatchdogFor(fix.engine(), *sys, kPollMs, 2500));
    sys->useWallTime();   // the measuring is done; requests need the audio thread
    flushLog(fix);
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().rateSkewRecoveryCount() == 3);

    // The user hears about it exactly once, by device name with both rates;
    // the detections themselves stay in the log as the record of why.
    const auto lines = fix.debugMessages();
    auto count = [&](const char* needle) {
        int n = 0;
        for (auto& l : lines) if (l.find(needle) != std::string::npos) ++n;
        return n;
    };
    CHECK(count("'Fake Speakers' reports 48000 Hz but is delivering ~") == 1);
    CHECK(count("rate skew detected") >= 1);
    CHECK(count("requesting the measured rate") == 1);
    CHECK(sessionRate(fix) == 44100);

    // Still playing, still answering.
    CHECK(fix.engine().engineState() == EngineState::Running);
    CHECK(fix.engine().audioSource() == ClockworkEngine::AudioSource::RealCallback);
    OscReply r;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r, 2000));
}

TEST_CASE("RateSkew: a device that skews at the measured rate too gets no further "
          "recoveries this session", "[RateSkew][Watchdog]") {
    auto sys = makeSimpleSystem();
    // Delivers 40k whatever it is opened at: nothing it advertises is true.
    sys->at("Fake Speakers").deliveredRate = 40000.0;
    EngineFixture fix(lyingDeviceConfig(sys));

    sys->useVirtualTime();
    // Two reopens, one adopt (44100, the nearest advertised to 40000)...
    REQUIRE(runWatchdogUntil(fix.engine(), *sys, kPollMs, 15000, [&] {
        return fix.engine().rateSkewRecoveryCount() >= 3 && sessionRate(fix) == 44100;
    }));

    // ...and at 44100 it still skews (0.907x). The policy gives up: the
    // count freezes, the engine keeps playing at the adopted rate.
    REQUIRE(runWatchdogFor(fix.engine(), *sys, kPollMs, 3000));
    sys->useWallTime();   // the measuring is done; requests need the audio thread
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().rateSkewRecoveryCount() == 3);
    CHECK(fix.engine().engineState() == EngineState::Running);
    CHECK(fix.engine().audioSource() == ClockworkEngine::AudioSource::RealCallback);
    OscReply r;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r, 2000));
}

TEST_CASE("RateSkew: an honest device is never recovered", "[RateSkew][Watchdog]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(lyingDeviceConfig(sys));
    sys->useVirtualTime();
    REQUIRE(runWatchdogFor(fix.engine(), *sys, kPollMs, 1500));   // five windows, once Live
    CHECK(fix.engine().rateSkewRecoveryCount() == 0);
    CHECK(sessionRate(fix) == 48000);
}

TEST_CASE("RateSkew: a device racing at nearly five times its rate is recovered, and "
          "the engine keeps playing", "[RateSkew][Watchdog]") {
    // The other way round from the mixer: a driver's timer free-running fast
    // (seen just before a wake), so every scheduled note comes early.
    auto sys = makeSimpleSystem();
    sys->at("Fake Speakers").deliveredRate = 230400.0;   // 4.8x its 48000
    EngineFixture fix(lyingDeviceConfig(sys));
    subscribe(fix);

    sys->useVirtualTime();
    const bool recovered = runWatchdogUntil(fix.engine(), *sys, kPollMs, 15000, [&] {
        for (auto& r : said(fix, CLOCKWORK_SYS("devices/reopen.done")))
            if (r.argInt(0) == 1) return true;
        return false;
    });
    INFO(fix.debugMessagesDump());
    REQUIRE(recovered);

    // Nothing it advertises is true either, so the engine gives up asking and
    // plays on at the nearest rate it has.
    REQUIRE(runWatchdogFor(fix.engine(), *sys, kPollMs, 3000));
    sys->useWallTime();
    CHECK(fix.engine().engineState() == EngineState::Running);
    OscReply r;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r, 2000));
}

TEST_CASE("RateSkew: the user changing the session's rate is not mistaken for a "
          "device keeping the wrong time", "[RateSkew][Watchdog]") {
    // The frames delivered at 48k, measured against 44.1k, would read as a
    // device running 9% fast.
    auto sys = makeSimpleSystem();                 // honest
    EngineFixture fix(lyingDeviceConfig(sys));
    subscribe(fix);

    sys->useVirtualTime();
    REQUIRE(runWatchdogFor(fix.engine(), *sys, kPollMs, 1000));   // measuring it
    sys->useWallTime();                            // a switch waits for the device's ticks
    REQUIRE(switchWhenFree(fix, "", 44100.0).success);
    sys->useVirtualTime();
    REQUIRE(runWatchdogFor(fix.engine(), *sys, kPollMs, 1500));
    sys->useWallTime();

    flushLog(fix);
    INFO(fix.debugMessagesDump());
    CHECK(said(fix, CLOCKWORK_SYS("devices/reopen.done")).empty());
    const auto rebuilt = said(fix, CLOCKWORK_SYS("setup"));
    REQUIRE(rebuilt.size() == 1);                  // the switch's own
    CHECK(rebuilt.front().argInt(0) == 44100);
    CHECK(sessionRate(fix) == 44100);
}
