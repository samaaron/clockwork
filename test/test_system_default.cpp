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
 * One rule on every platform, looked at afresh each time anything about the
 * devices changes: in system mode, the engine plays on the default. An OS
 * says the default moved in its own way — macOS with a notification of its
 * own, Windows as a change to the device list — and both start the same pass
 * on the device lane, which reads the default and the device it plays on and
 * acts only if they differ. A change is never dropped: not because the engine
 * has just switched devices itself (a notification that switch provoked finds
 * nothing to do), not because a swap holds the devices (the pass waits, then
 * looks again), and not because the engine is still booting.
 *
 * The default is the fake's (FakeSystem's defaultDeviceIndex): under the
 * factory boundary the engine asks the factory's device type, not the HAL.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <chrono>
#include <future>
#include <memory>
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;
using fake_audio::FakeDeviceSpec;
using fake_audio::FakeSystem;

namespace {

// Everything the device lane was given before this has run: it runs its
// tasks in order.
void laneDone(ClockworkEngine& engine) {
    std::promise<void> done;
    auto finished = done.get_future();
    engine.postDeviceTask([&] { done.set_value(); });
    REQUIRE(finished.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
}

bool playingOn(EngineFixture& fix, const std::string& name, int timeoutMs = 10000) {
    return fix.pollUntil([&] { return fix.engine().currentDevice().name == name; },
                         timeoutMs);
}

// The OS moves its default. The engine reads it under the gate, so this
// writes it there.
void setDefault(EngineFixture& fix, FakeSystem& sys, int outputIndex) {
    auto hold = fix.engine().testHoldSwapGate();
    sys.types[0].defaultDeviceIndex = outputIndex;
}

// Headphones, not plugged in yet: outputs are then Fake Speakers, Fake
// Interface, Fake Headphones.
std::shared_ptr<FakeDeviceSpec> addHeadphones(FakeSystem& sys) {
    auto phones = std::make_shared<FakeDeviceSpec>();
    phones->name = "Fake Headphones";
    phones->maxInputChannels = 0;
    phones->hidden = true;
    sys.types[0].devices.push_back(phones);
    return phones;
}

}  // namespace

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

TEST_CASE("SystemDefault: in system mode the engine follows the default every "
          "time it moves, however soon after its own switch", "[SystemDefault]") {
    auto sys = makeSimpleSystem();
    sys->types[0].defaultDeviceIndex = 1;     // outputs: Fake Speakers, Fake Interface
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());
    REQUIRE(fix.engine().currentDevice().name == "Fake Interface");

    // Each move comes straight after the engine's own switch: the one into
    // system mode, then each follow. There is no quiet window after a switch
    // for a real move to fall into.
    // The headphones come out: the default is the speakers...
    setDefault(fix, *sys, 0);
    fix.engine().testSystemDefaultOutputChanged();
    CHECK(playingOn(fix, "Fake Speakers"));
    // ...and they go back in.
    setDefault(fix, *sys, 1);
    fix.engine().testSystemDefaultOutputChanged();
    INFO(fix.debugMessagesDump());
    CHECK(playingOn(fix, "Fake Interface"));
    CHECK(fix.engine().preferredOutputDevice().empty());
}

TEST_CASE("SystemDefault: a default that moves while a swap holds the devices is "
          "followed once the swap is done", "[SystemDefault]") {
    auto sys = makeSimpleSystem();
    sys->types[0].defaultDeviceIndex = 1;
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());
    REQUIRE(fix.engine().currentDevice().name == "Fake Interface");

    {
        auto hold = fix.engine().testHoldSwapGate();   // a swap, longer than any real one
        sys->types[0].defaultDeviceIndex = 0;
        fix.engine().testSystemDefaultOutputChanged();
        // The pass waits its bounded turn, then says it gave up waiting.
        REQUIRE(fix.pollUntil([&] {
            return fix.debugMessagesDump().find("gate busy") != std::string::npos; },
            10000));
    }
    INFO(fix.debugMessagesDump());
    CHECK(playingOn(fix, "Fake Speakers"));
}

TEST_CASE("SystemDefault: a default that moves with the device list is followed "
          "on the list change alone", "[SystemDefault]") {
    // Windows says nothing of its own when the default moves: the list
    // changes (WASAPI lists the default first), and that is all.
    auto sys = makeSimpleSystem();
    auto phones = addHeadphones(*sys);
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());
    REQUIRE(fix.engine().currentDevice().name == "Fake Speakers");

    {   // headphones in, and the OS makes them the default
        auto hold = fix.engine().testHoldSwapGate();
        phones->hidden = false;
        sys->types[0].defaultDeviceIndex = 2;
    }
    REQUIRE(sys->reportListChanged());
    INFO(fix.debugMessagesDump());
    CHECK(playingOn(fix, "Fake Headphones"));
    CHECK(fix.engine().preferredOutputDevice().empty());
}

TEST_CASE("SystemDefault: a chosen output stays put when the default moves",
          "[SystemDefault]") {
    auto sys = makeSimpleSystem();               // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));   // -H is a pick
    setDefault(fix, *sys, 1);
    setDefault(fix, *sys, 0);
    fix.engine().testSystemDefaultOutputChanged();
    REQUIRE(sys->reportListChanged());
    laneDone(fix.engine());
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().currentDevice().name == "Fake Interface");
    CHECK(fix.engine().preferredOutputDevice() == "Fake Interface");
}

TEST_CASE("SystemDefault: a default that will not open is tried once each time "
          "it moves, not on every change", "[SystemDefault]") {
    // On macOS a failed switch builds and pulls down an aggregate device, and
    // the list changes again: retrying on every change would never stop.
    auto sys = makeSimpleSystem();
    auto phones = addHeadphones(*sys);
    phones->failOpen = true;
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());
    REQUIRE(fix.engine().currentDevice().name == "Fake Speakers");

    {
        auto hold = fix.engine().testHoldSwapGate();
        phones->hidden = false;
        sys->types[0].defaultDeviceIndex = 2;
    }
    REQUIRE(sys->reportListChanged());
    laneDone(fix.engine());
    const int tried = phones->opens.load();
    INFO(fix.debugMessagesDump());
    REQUIRE(tried > 0);

    for (int i = 0; i < 3; ++i) REQUIRE(sys->reportListChanged());
    fix.engine().testSystemDefaultOutputChanged();
    laneDone(fix.engine());
    CHECK(phones->opens.load() == tried);
    CHECK(fix.waitForBlocks(20));                // still playing, somewhere

    // The default moves away and back: a move, so it is tried again.
    setDefault(fix, *sys, 0);
    fix.engine().testSystemDefaultOutputChanged();
    laneDone(fix.engine());
    setDefault(fix, *sys, 2);
    fix.engine().testSystemDefaultOutputChanged();
    laneDone(fix.engine());
    CHECK(phones->opens.load() > tried);
}

TEST_CASE("SystemDefault: a default that moves while the engine boots is followed "
          "once it is running", "[SystemDefault]") {
    auto sys = makeSimpleSystem();               // the default is Fake Speakers
    auto cfg = fakeEngineConfig(sys, "");        // system mode
    EngineFixture fix(cfg, [&](ClockworkEngine& engine) {
        // Just before the engine is running: the default moves, the OS says
        // so, and the device lane takes the news in.
        engine.testInitFailure = [&engine, sys] {
            sys->types[0].defaultDeviceIndex = 1;
            engine.testSystemDefaultOutputChanged();
            laneDone(engine);
            return std::string();
        };
    });
    const bool followed = playingOn(fix, "Fake Interface");
    INFO(fix.debugMessagesDump());
    INFO("playing on " << fix.engine().currentDevice().name);
    CHECK(followed);
}
