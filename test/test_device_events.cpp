// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_events.cpp — device events arriving whenever the OS sends them.
 *
 * An OS tells the device layer about a device coming or going on a thread of
 * its own, at a moment of its own; the engine must be safe whatever it is in
 * the middle of. Sonic Pi 5.0.0 (SuperSonic 0.71.0) crashed twice in two days
 * on exactly this: SIGSEGV in the device manager's own list-change handling
 * as headphones were unplugged (2026-10-02, copying currentSetup out of a
 * manager whose memory was gone), and SIGSEGV in a message-loop callback
 * after a Bluetooth headset went away (2026-10-03). Each time the audio
 * engine process died: the device manager closed and reopened devices by
 * itself on the thread the change arrived on, while the engine's device lane
 * was rebuilding that same manager.
 *
 * The device layer now hands every change to the engine, and the device lane
 * decides what it means: the device that was playing has gone, the user's
 * device is back, or only the list to report has changed. Run the race case
 * under AddressSanitizer (build-asan) too: a use of freed memory shows there
 * with both threads' stacks even when the timing does not happen to crash.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

// Booted, and subscribed to the engine's broadcasts as a GUI is.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

// Whether the engine has told its clients it is playing on `name`: the
// device report a GUI fills its output menu from.
bool reported(EngineFixture& fix, const std::string& name) {
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("devices") && r.parsed().argString(1) == name)
            return true;
    return false;
}

bool playingOn(EngineFixture& fix, const std::string& name, int timeoutMs = 10000) {
    return fix.pollUntil([&] { return fix.engine().currentDevice().name == name; },
                         timeoutMs);
}

// Whether the engine has told its clients it records from `name`: the report
// a GUI fills its input menu from.
bool reportedInput(EngineFixture& fix, const std::string& name) {
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("input-devices") && r.parsed().argString(0) == name)
            return true;
    return false;
}

bool recordingFrom(EngineFixture& fix, const std::string& output,
                   const std::string& input, int timeoutMs = 10000) {
    return fix.pollUntil([&] {
        const auto cur = fix.engine().currentDevice();
        return cur.name == output && cur.inputDeviceName == input
            && cur.activeInputChannels > 0;
    }, timeoutMs);
}

// Everything the engine has sent so far has arrived: this reply comes back
// through the same ring, after it.
void flushReplies(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r, 30000));
}

std::shared_ptr<fake_audio::FakeDeviceSpec> device(const std::string& name, int outs, int ins) {
    auto d = std::make_shared<fake_audio::FakeDeviceSpec>();
    d->name = name;
    d->maxOutputChannels = outs;
    d->maxInputChannels  = ins;
    return d;
}

}  // namespace

TEST_CASE("DeviceEvents: unplugging the device the engine plays on moves it to "
          "the default at once, and its clients are told", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();                // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);

    sys->device("Fake Interface")->hidden = true;  // unplugged
    REQUIRE(sys->reportListChanged());

    const bool moved = playingOn(fix, "Fake Speakers");
    INFO(fix.debugMessagesDump());
    REQUIRE(moved);
    CHECK(fix.pollUntil([&] { return reported(fix, "Fake Speakers"); }, 10000));
    CHECK(fix.waitForBlocks(20));
    // Still the user's device: it is where the engine goes when it is back.
    CHECK(fix.engine().preferredOutputDevice() == "Fake Interface");
}

TEST_CASE("DeviceEvents: the user's device plugged back in is played on again, "
          "and its clients are told", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);
    auto interface = sys->device("Fake Interface");

    interface->hidden = true;
    REQUIRE(sys->reportListChanged());
    REQUIRE(playingOn(fix, "Fake Speakers"));
    fix.clearReplies();

    interface->hidden = false;                     // plugged back in
    REQUIRE(sys->reportListChanged());

    const bool back = playingOn(fix, "Fake Interface");
    INFO(fix.debugMessagesDump());
    REQUIRE(back);
    CHECK(fix.pollUntil([&] { return reported(fix, "Fake Interface"); }, 10000));
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("DeviceEvents: the user's microphone plugged back in is recorded from again, "
          "and one already recording is left alone", "[DeviceEvents]") {
    auto sys = std::make_shared<fake_audio::FakeSystem>();
    auto speakers = device("Fake Speakers", 2, 0);
    auto mic = device("Fake Microphone", 0, 2);   // no other input to stand in for it
    mic->hidden = true;
    sys->types.push_back({ "FakeDriver", { speakers, mic }, 0 });
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.numInputChannels = 2;
    cfg.inputDevice = "Fake Microphone";          // saved, and not plugged in
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeInputChannels == 0);
    subscribe(fix);

    mic->hidden = false;
    REQUIRE(sys->reportListChanged());
    const bool recording = recordingFrom(fix, "Fake Speakers", "Fake Microphone");
    INFO(fix.debugMessagesDump());
    REQUIRE(recording);
    CHECK(fix.engine().currentDevice().activeInputChannels == 2);
    CHECK(fix.pollUntil([&] { return reportedInput(fix, "Fake Microphone"); }, 10000));

    // The list changing again while it records costs it nothing: the devices
    // are not opened again (a pair opens through its output) and nothing is
    // rebuilt.
    deviceLaneDone(fix.engine());
    flushReplies(fix);
    fix.clearReplies();
    const int opened = speakers->opens.load();
    REQUIRE(sys->reportListChanged());
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    flushReplies(fix);
    int rebuilds = 0;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("setup")) ++rebuilds;
    CHECK(rebuilds == 0);
    CHECK(speakers->opens.load() == opened);
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("DeviceEvents: the device list changing while recovery rebuilds the "
          "device manager is safe", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.watchdogRecoveryCooldownMs = 0;   // recover as fast as asked
    EngineFixture fix(cfg);
    auto speakers = sys->device("Fake Speakers");

    // The OS: the open device goes and comes back (headphones out, in), and
    // each time it says so, whenever it likes.
    std::atomic<bool> stop { false };
    std::atomic<int> reports { 0 };
    std::thread os([&] {
        std::mt19937 rng(20261003);
        while (!stop.load()) {
            if (rng() % 3 == 0) speakers->hidden = !speakers->hidden.load();
            if (sys->reportListChanged()) reports.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
        }
    });

    // Meanwhile recovery rebuilds the manager, as Reset and the watchdog do.
    std::mt19937 rng(42);
    int recoveries = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until) {
        std::string reason;
        if (fix.engine().requestAudioRecovery(reason)) ++recoveries;
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + rng() % 40));
    }
    stop = true;
    os.join();
    speakers->hidden = false;

    INFO("reports=" << reports.load() << " recoveries=" << recoveries);
    CHECK(reports.load() > 0);
    CHECK(recoveries > 0);

    // And the engine is still answering.
    OscReply reply;
    const bool answered = fix.pollUntil([&] {
        fix.send(osc_test::message("/dummy/ping"));
        return fix.waitForReply("/dummy/pong", reply, 200);
    }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(answered);
}

// ── The device played on, changing where it stands ──────────────────────────
// It can die while still listed, or its driver can change its channels (an
// interface's control panel turning outputs on or off). The OS says so on a
// thread of its own; the engine looks at the device as its driver reports
// it now and acts where that differs from what it is playing. The device
// layer no longer restarts the device by itself behind the engine's back.

TEST_CASE("DeviceEvents: the device played on dying where it stands is left "
          "for one that plays", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();               // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));

    sys->device("Fake Interface")->dead = true;  // still listed, gone all the same
    REQUIRE(sys->reportOpenDeviceChanged());

    const bool moved = playingOn(fix, "Fake Speakers");
    INFO(fix.debugMessagesDump());
    REQUIRE(moved);
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("DeviceEvents: the device played on losing channels is played on "
          "with the channels it has", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    auto interface = sys->device("Fake Interface");
    interface->maxOutputChannels = 4;
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.numOutputChannels = 4;
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeOutputChannels == 4);

    {   // its control panel turns two of its outputs off
        auto hold = fix.engine().testHoldSwapGate();
        interface->maxOutputChannels = 2;
    }
    REQUIRE(sys->reportOpenDeviceChanged());

    const bool narrowed = fix.pollUntil(
        [&] { return fix.engine().currentDevice().activeOutputChannels == 2; }, 10000);
    INFO(fix.debugMessagesDump());
    CHECK(narrowed);
    CHECK(fix.engine().currentDevice().name == "Fake Interface");
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("DeviceEvents: the engine's own switch, reported back by the device, "
          "rebuilds nothing more", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);
    REQUIRE(switchWhenFree(fix, "Fake Interface", 44100.0).success);
    OscReply ownRebuild;                         // the switch's own
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("setup"), ownRebuild, 10000));
    deviceLaneDone(fix.engine());
    fix.clearReplies();

    // What CoreAudio says after a switch: the rate, the buffer, the format.
    for (int i = 0; i < 3; ++i) REQUIRE(sys->reportOpenDeviceChanged());
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    INFO(fix.debugMessagesDump());
    int rebuilds = 0;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("setup")) ++rebuilds;
    CHECK(rebuilds == 0);
    CHECK(fix.waitForBlocks(20));
}
