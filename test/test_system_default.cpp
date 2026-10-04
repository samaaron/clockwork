// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_system_default.cpp — system mode follows the system default, and
 * following it is not choosing it.
 *
 * Seen at a gig (2026-10-02, Sonic Pi 5.0.0 on macOS): in system mode the
 * engine followed the default to the built-in speakers, and that follow went
 * through switchDevice as if the user had picked them — so the speakers became
 * the pinned output. When the headphones were plugged in, the default moved
 * to them and the engine logged "pinned='MacBook Pro Speakers'; not following":
 * the sound stayed on the speakers while the headphone jack, and the PA on it,
 * stayed silent. Every follow after the first was refused the same way.
 *
 * The default is the fake's (FakeSystem's defaultDeviceIndex): under the
 * factory boundary the engine asks the factory's device type, not the HAL.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <chrono>
#include <string>
#include <thread>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

TEST_CASE("SystemDefault: choosing the system default pins nothing",
          "[SystemDefault]") {
    auto sys = makeSimpleSystem();            // the default is Fake Speakers
    // It plays only at 44.1k and the session is at 48k, so the system path
    // has to switch on every platform: by name on macOS, and elsewhere
    // through JUCE's default and a cold swap to its rate.
    sys->device("Fake Speakers")->sampleRates = { 44100.0 };
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    REQUIRE(fix.engine().preferredOutputDevice() == "Fake Interface");   // -H is a pick

    const auto err = fix.engine().setDeviceMode("system");
    INFO(fix.debugMessagesDump());
    CHECK(err.empty());
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    CHECK(fix.engine().preferredOutputDevice().empty());
}

#ifdef __APPLE__
// Following the default as it moves is macOS's: the HAL tells the engine when
// it changes (handleSystemDefaultOutputChanged).
TEST_CASE("SystemDefault: in system mode the engine follows the default every "
          "time it moves", "[SystemDefault]") {
    auto sys = makeSimpleSystem();
    sys->types[0].defaultDeviceIndex = 1;     // outputs: Fake Speakers, Fake Interface
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());
    REQUIRE(fix.engine().currentDevice().name == "Fake Interface");

    auto moveDefaultTo = [&](int index) {
        {   // the engine reads the default under the gate; so does this
            auto hold = fix.engine().testHoldSwapGate();
            sys->types[0].defaultDeviceIndex = index;
        }
        // The handler ignores a change within 2 s of the engine's own swap:
        // a swap provokes default-change notifications of its own.
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        fix.engine().testSystemDefaultOutputChanged();
    };

    // The headphones come out: the default is the speakers...
    moveDefaultTo(0);
    CHECK(fix.pollUntil([&] {
        return fix.engine().currentDevice().name == "Fake Speakers"; }, 5000));
    // ...and they go back in.
    moveDefaultTo(1);
    INFO(fix.debugMessagesDump());
    CHECK(fix.pollUntil([&] {
        return fix.engine().currentDevice().name == "Fake Interface"; }, 5000));
    CHECK(fix.engine().preferredOutputDevice().empty());
}
#endif
