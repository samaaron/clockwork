// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_no_audio_device.cpp — the engine with no audio device to open.
 *
 * No device: the engine is idle (no audio source, nothing ticking), says it is
 * waiting, and plays as soon as a device appears — at once when the OS reports
 * one, and through the watchdog's own looking when the OS says nothing (ALSA
 * reports no device changes at all). It never fakes a session on a silent
 * driver: headless is an explicit mode only (Config::headless).
 *
 * The machine is a fake one whose devices are all unplugged, so the device
 * manager exists and initialises and opens nothing — what PipeWire's "no
 * channels" default sink did to a JUCE ALSA boot (#3526) — and nothing on the
 * machine running the case can reach it.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <atomic>
#include <memory>
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

// A machine with every device unplugged.
std::shared_ptr<fake_audio::FakeSystem> machineWithNothingPluggedIn() {
    auto sys = makeSimpleSystem();
    for (auto& d : sys->types[0].devices) d->hidden = true;
    return sys;
}

uint32_t processCount(ClockworkEngine& e) {
    return e.audioCallback().processCount.load(std::memory_order_acquire);
}

bool playingOn(EngineFixture& fix, const std::string& name, int timeoutMs = 10000) {
    return fix.pollUntil([&] { return fix.engine().currentDevice().name == name; },
                         timeoutMs);
}

}  // namespace

TEST_CASE("NoAudioDevice: with no device to open, the engine waits for one and "
          "plays nothing meanwhile", "[NoAudioDevice]") {
    auto sys = machineWithNothingPluggedIn();
    EngineFixture fix(fakeEngineConfig(sys, ""));        // system mode

    CHECK(fix.engine().isRunning());
    CHECK(fix.engine().waitingForAudioDevice());
    CHECK(fix.engine().audioSource() == ClockworkEngine::AudioSource::None);
    CHECK(fix.engine().currentDevice().name.empty());
    CHECK(fix.engine().currentDevice().activeSampleRate == 0.0);

    // A change to the devices that brings none finds nothing to open.
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    CHECK(fix.engine().waitingForAudioDevice());
    CHECK(processCount(fix.engine()) == 0);
}

TEST_CASE("NoAudioDevice: a device plugged in while the engine waits is played "
          "on as soon as the OS says so", "[NoAudioDevice]") {
    auto sys = machineWithNothingPluggedIn();
    EngineFixture fix(fakeEngineConfig(sys, ""));
    REQUIRE(fix.engine().waitingForAudioDevice());

    sys->device("Fake Speakers")->hidden = false;        // plugged in, the default
    REQUIRE(sys->reportListChanged());

    const bool playing = playingOn(fix, "Fake Speakers");
    INFO(fix.debugMessagesDump());
    REQUIRE(playing);
    CHECK_FALSE(fix.engine().waitingForAudioDevice());
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("NoAudioDevice: a device plugged in while the engine waits is found "
          "by the watchdog when the OS says nothing", "[NoAudioDevice]") {
    auto sys = machineWithNothingPluggedIn();
    auto clockMs = std::make_shared<std::atomic<int64_t>>(0);
    auto cfg = fakeEngineConfig(sys, "");
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;
    cfg.watchdogPollMs   = 50;
    cfg.watchdogClockMs  = [clockMs] { return clockMs->load(); };   // polled by the case
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().waitingForAudioDevice());

    sys->device("Fake Speakers")->hidden = false;        // and no word of it
    clockMs->fetch_add(50);
    fix.engine().watchdogPoll();

    const bool playing = playingOn(fix, "Fake Speakers");
    INFO(fix.debugMessagesDump());
    REQUIRE(playing);
    CHECK(fix.waitForBlocks(20));
}

// A swap holds the gate across a moment with no audio source (stop, rebuild,
// start). A watchdog that took that moment for "waiting for a device" would
// recover onto the system default and undo the switch. The case polls the
// watchdog itself, so every poll sees the swap in flight.
TEST_CASE("NoAudioDevice: the watchdog leaves a device swap in flight alone",
          "[NoAudioDevice]") {
    auto sys = machineWithNothingPluggedIn();
    auto clockMs = std::make_shared<std::atomic<int64_t>>(0);
    auto cfg = fakeEngineConfig(sys, "");
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 100;
    cfg.watchdogPollMs   = 20;
    cfg.watchdogClockMs  = [clockMs] { return clockMs->load(); };
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().waitingForAudioDevice());

    auto swap = fix.engine().testHoldSwapGate();
    sys->device("Fake Speakers")->hidden = false;        // there to recover onto
    for (int i = 0; i < 20; ++i) {
        clockMs->fetch_add(20);
        fix.engine().watchdogPoll();
    }
    CHECK(fix.engine().watchdogRecoveryCount() == 0);
}
