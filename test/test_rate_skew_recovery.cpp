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
 * The fake device lies the same way (FakeDeviceSpec::deliveredRate), the
 * windows are short, and the recovery cooldown is short, so a session that
 * took the mixer ten minutes plays out here in a few seconds.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

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

} // namespace

TEST_CASE("RateSkew: a device that reports 48k and delivers 44.1k is recovered at "
          "most N times, ends at the measured rate, and keeps playing",
          "[RateSkew][Watchdog]") {
    auto sys = makeSimpleSystem();
    sys->device("Fake Speakers")->deliveredRate = 44100.0;   // reports 48000, ticks at 44100
    EngineFixture fix(lyingDeviceConfig(sys));

    SwapLog log;
    fix.engine().onSwapEvent = [&](const std::string& event, const SwapResult& r) {
        if (event.rfind("swap:", 0) == 0 || event.find("recover") != std::string::npos) {
            std::lock_guard<std::mutex> lk(log.mu);
            log.swaps.push_back(r);
        }
    };

    REQUIRE(static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 48000);

    // Three recoveries: two plain reopens, then the adopt. Each needs two bad
    // 300 ms windows plus the swap and the cooldown.
    REQUIRE(fix.pollUntil([&] {
        return fix.engine().rateSkewRecoveryCount() >= 3;
    }, 15000));

    // The adopt landed the session on the rate the device actually keeps
    // time at — which it advertises, so the request snapped to it exactly.
    REQUIRE(fix.pollUntil([&] {
        return static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 44100;
    }, 5000));

    // From here the device is honest against its new nominal rate: no further
    // recoveries. Wait several verdict periods to prove the storm is over.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
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
    CHECK(static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 44100);

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
    sys->device("Fake Speakers")->deliveredRate = 40000.0;
    EngineFixture fix(lyingDeviceConfig(sys));

    // Two reopens, one adopt (44100, the nearest advertised to 40000)...
    REQUIRE(fix.pollUntil([&] {
        return fix.engine().rateSkewRecoveryCount() >= 3;
    }, 15000));
    REQUIRE(fix.pollUntil([&] {
        return static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 44100;
    }, 5000));

    // ...and at 44100 it still skews (0.907x). The policy gives up: the
    // count freezes, the engine keeps playing at the adopted rate.
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
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
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));   // five windows
    CHECK(fix.engine().rateSkewRecoveryCount() == 0);
    CHECK(static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 48000);
}
