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
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include "DeviceInvariants.h"
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <atomic>
#include <memory>
#include <string>
#include <tuple>

using Catch::Generators::from_range;
using fake_audio::Driver;
using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

// A machine with every device unplugged, on `driver`.
std::shared_ptr<fake_audio::FakeSystem> machineWithNothingPluggedIn(
        Driver driver = Driver::CoreAudio) {
    auto sys = makeSimpleSystem();
    fake_audio::behaveLike(sys->types[0], driver);
    for (auto& d : sys->types[0].devices) d->hidden = true;
    return sys;
}

// The watchdog on the case's clock: it looks only when the case polls it.
ClockworkEngine::Config watchedByTheCase(std::shared_ptr<fake_audio::FakeSystem> sys,
                                         std::shared_ptr<std::atomic<int64_t>> clockMs) {
    auto cfg = fakeEngineConfig(std::move(sys), "");
    cfg.callbackWatchdog = true;
    cfg.watchdogStallMs  = 250;
    cfg.watchdogPollMs   = 50;
    cfg.watchdogClockMs  = [clockMs] { return clockMs->load(); };
    return cfg;
}

uint32_t processCount(ClockworkEngine& e) {
    return e.audioCallback().processCount.load(std::memory_order_acquire);
}


}  // namespace

TEST_CASE("NoAudioDevice: with no device to open, the engine waits for one and "
          "plays nothing meanwhile", "[NoAudioDevice]") {
    // Once for each way a list change is heard: at once, or not at all.
    const Driver driver = GENERATE(from_range(fake_audio::onePerWay(
        fake_audio::anyDriver, [](const auto& t) { return t.listChanges; })));
    CAPTURE(fake_audio::driverName(driver));
    auto sys = machineWithNothingPluggedIn(driver);
    EngineFixture fix(fakeEngineConfig(sys, ""));        // system mode

    CHECK(fix.engine().isRunning());
    CHECK(fix.engine().waitingForAudioDevice());
    CHECK(fix.engine().audioSource() == ClockworkEngine::AudioSource::None);
    CHECK(fix.engine().currentDevice().name.empty());
    CHECK(fix.engine().currentDevice().activeSampleRate == 0.0);
    checkCoherent(fix);

    // A change to the devices that brings none finds nothing to open.
    sys->reportListChanged();
    checkCoherent(fix);
    CHECK(fix.engine().waitingForAudioDevice());
    CHECK(processCount(fix.engine()) == 0);
}

// For each way the drivers differ here (whether the OS says a device came,
// and whether there is a default to follow: CoreAudio for the drivers that
// hear and name one, ASIO, ALSA), wired or wireless (headphones that connect
// over Bluetooth): a driver may open the two in different ways, and the
// engine must play on what it opens either way. The watchdog looks only when
// the driver hears nothing, so where the OS says so it is the engine's answer
// to the OS that is tested, not a recovery that would cover for it. Linux CI
// caught the engine opening the device and then waiting on (2026-10-05),
// which no Mac run could: only CoreAudio's way was ever exercised here.
TEST_CASE("NoAudioDevice: a device plugged in while the engine waits is played "
          "on, at once when the OS says so and found by the watchdog when it "
          "does not", "[NoAudioDevice]") {
    const Driver driver = GENERATE(from_range(fake_audio::onePerWay(
        fake_audio::anyDriver,
        [](const auto& t) { return std::make_tuple(t.listChanges, t.namesDefault); })));
    const bool wireless = GENERATE(false, true);
    CAPTURE(fake_audio::driverName(driver), wireless);

    auto sys = machineWithNothingPluggedIn(driver);
    auto speakers = sys->device("Fake Speakers");        // the default, once here
    speakers->wireless = wireless;
    auto clockMs = std::make_shared<std::atomic<int64_t>>(0);
    EngineFixture fix(watchedByTheCase(sys, clockMs));
    REQUIRE(fix.engine().waitingForAudioDevice());
    checkCoherent(fix);

    speakers->hidden = false;                            // plugged in
    if (!sys->reportListChanged()) {
        clockMs->fetch_add(50);
        fix.engine().watchdogPoll();
    }
    checkPlayingOn(fix, "Fake Speakers");
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
