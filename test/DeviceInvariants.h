// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * DeviceInvariants.h — what holds of an engine on fake devices once a device
 * event has been dealt with, whatever the driver and whatever happened: its
 * account of itself agrees with what is open.
 *
 *   a device is open  <=>  its callback is the engine's source
 *                     <=>  the engine is not waiting for a device
 *                      =>  blocks keep coming
 *
 * The two cases Linux CI failed on 2026-10-05 broke it: the engine named the
 * device it had opened, still said it was waiting for one, and rendered
 * nothing. A case that checks this after each step catches that wherever it
 * happens, not only where a case thought to look.
 *
 * Fake-device engines only: a headless or manual-pump engine has no device by
 * design.
 */
#pragma once

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_prefix.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

// The device work the last event started has finished: the lane has run what
// it was given, and nothing it queued behind itself (a recovery) is left.
inline void deviceWorkDone(EngineFixture& fix) {
    auto& e = fix.engine();
    deviceLaneDone(e);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (e.devicePhase() != ClockworkEngine::DevicePhase::Idle || e.recoveryInFlight()) {
        REQUIRE(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    deviceLaneDone(e);
}

inline void checkCoherent(EngineFixture& fix) {
    deviceWorkDone(fix);
    auto& e = fix.engine();
    const std::string device = e.currentDevice().name;
    const bool open = !device.empty();
    INFO("device: '" << device << "'");
    INFO(fix.debugMessagesDump());
    CHECK((e.audioSource() == ClockworkEngine::AudioSource::RealCallback) == open);
    CHECK(e.waitingForAudioDevice() == !open);
    if (open) CHECK(fix.waitForBlocks(20));
}

// The user picks an output from the menu; what the engine says it did.
inline osc_test::ParsedReply pick(EngineFixture& fix, const char* output) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << output << 0.0f << static_cast<osc::int32>(0) << "";
    fix.send(b.end());
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), done, 10000));
    return done.parsed();
}

// Playing on `name`, and coherent about it.
inline void checkPlayingOn(EngineFixture& fix, const std::string& name,
                           int timeoutMs = 10000) {
    const bool there = fix.pollUntil(
        [&] { return fix.engine().currentDevice().name == name; }, timeoutMs);
    INFO("playing on: '" << fix.engine().currentDevice().name << "'");
    INFO(fix.debugMessagesDump());
    REQUIRE(there);
    checkCoherent(fix);
}
