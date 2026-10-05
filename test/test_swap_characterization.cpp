// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_swap_characterization.cpp — pins the device swap subsystem's CURRENT
 * behaviour against fake devices (FakeAudioDevice.h), warts included.
 *
 * These are characterization tests in the Feathers sense: they are the
 * safety net under the switchDevice/enableInputChannels/switchDriver/
 * recovery refactor. When a later phase changes one of these behaviours
 * ON PURPOSE, update the pinned expectation in the same commit and say
 * why in its message. A surprise failure here means the refactor changed
 * behaviour it didn't mean to.
 *
 * Everything runs against a real juce::AudioDeviceManager whose only
 * device types are fakes — manager logic (name validation, setup
 * resolution, type switching) is JUCE's own. First suite in the repo to
 * exercise these paths off-hardware; before the deviceManagerFactory
 * seam every engine device test ran with a null manager.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <memory>
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;
using fake_audio::FakeDeviceSpec;
using fake_audio::FakeSystem;

namespace {

// Booted, and subscribed to the engine's broadcasts as a GUI is.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

// The user picks an output from the menu; what the engine says it did.
osc_test::ParsedReply pick(EngineFixture& fix, const char* output) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << output << 0.0f << static_cast<osc::int32>(0) << "";
    fix.send(b.end());
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), done, 10000));
    return done.parsed();
}

// Everything the engine has sent so far has arrived: this reply comes back
// through the same ring, after it.
void flushReplies(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r, 30000));
}

// /clockwork/setup: the DSP was rebuilt, and a client re-establishes what it
// held there.
int rebuilds(EngineFixture& fix) {
    int n = 0;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("setup")) ++n;
    return n;
}

std::shared_ptr<FakeDeviceSpec> addOutput(FakeSystem& sys, const std::string& name,
                                          int channels) {
    auto out = std::make_shared<FakeDeviceSpec>();
    out->name = name;
    out->maxOutputChannels = channels;
    out->maxInputChannels  = 0;
    sys.types[0].devices.push_back(out);
    return out;
}

}  // namespace

// ── Boot ────────────────────────────────────────────────────────────────────

TEST_CASE("Swap: -H boot opens the named fake device and ticks",
          "[SwapChar][boot]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto cur = fix.engine().currentDevice();
    REQUIRE(cur.name == "Fake Speakers");

    // The engine is live: OSC round-trips via real fake-device callbacks.
    OscReply reply;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

// ── Named device switch ─────────────────────────────────────────────────────

TEST_CASE("Swap: named same-driver switch succeeds and reports the device",
          "[SwapChar]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("Fake Interface", 0, 0, false, "__none__");
    REQUIRE(r.success);
    REQUIRE(r.deviceName == "Fake Interface");
    REQUIRE(fix.engine().currentDevice().name == "Fake Interface");
}

TEST_CASE("Swap: a device that plays the session's rate is switched to without a "
          "rebuild, and one that doesn't is opened at its nearest rate", "[SwapChar]") {
    auto sys = makeSimpleSystem();
    addOutput(*sys, "Fake 96k Box", 2)->sampleRates = { 44100.0, 96000.0 };
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));   // at 48000
    subscribe(fix);

    CHECK(pick(fix, "Fake Interface").argInt(0) == 1);
    deviceLaneDone(fix.engine());
    flushReplies(fix);
    CHECK(rebuilds(fix) == 0);
    CHECK(fix.engine().currentDevice().activeSampleRate == 48000.0);

    // No 48k: the nearest it has, and the engine rebuilt for it — running the
    // DSP at 48k against a 44.1k device plays everything flat and slow.
    INFO(fix.debugMessagesDump());
    CHECK(pick(fix, "Fake 96k Box").argInt(0) == 1);
    OscReply rebuilt;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("setup"), rebuilt, 10000));
    CHECK(rebuilt.parsed().argInt(0) == 44100);
    CHECK(fix.engine().currentDevice().activeSampleRate == 44100.0);
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("Swap: switching to a device with more outputs rebuilds the engine for all "
          "of them", "[SwapChar]") {
    // A switch that kept the DSP would keep it writing the two channels it was
    // built with, and channels three to eight would stay silent.
    auto sys = makeSimpleSystem();
    addOutput(*sys, "Fake 8out", 8);
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));   // two outputs
    subscribe(fix);

    const auto done = pick(fix, "Fake 8out");
    INFO(fix.debugMessagesDump());
    CHECK(done.argInt(0) == 1);
    OscReply rebuilt;
    CHECK(fix.waitForReply(CLOCKWORK_SYS("setup"), rebuilt, 10000));
    CHECK(fix.engine().currentDevice().activeOutputChannels == 8);
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("Swap: unknown output name is refused before any mutation",
          "[SwapChar]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("Ghost Device", 0, 0, false, "__none__");
    REQUIRE(!r.success);
    REQUIRE(!r.error.empty());
    // The device we were on is untouched.
    REQUIRE(fix.engine().currentDevice().name == "Fake Speakers");
}

TEST_CASE("Swap: rate change forces a cold swap at the requested rate",
          "[SwapChar]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("", 44100, 0, false, "__none__");
    REQUIRE(r.success);
    REQUIRE(r.type == SwapType::Cold);
    REQUIRE(r.sampleRate == 44100.0);

    // Engine still responsive after the guest rebuild.
    OscReply reply;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

// ── Input enable / disable round-trip ───────────────────────────────────────

TEST_CASE("Swap: inputs enable via saved device name, disable via __none__",
          "[SwapChar][inputs]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    // Seed mLastInputDeviceName with an explicit full-duplex switch.
    auto r = switchWhenFree(fix, "Fake Interface", 0, 0, false, "Fake Interface");
    REQUIRE(r.success);
    REQUIRE(r.inputDeviceName == "Fake Interface");

    // Disable...
    auto off = whenSwapGateFree([&] { return fix.engine().enableInputChannels(0); });
    REQUIRE(off.success);

    // ...and re-enable through the saved name — no live CoreAudio needed.
    auto on = whenSwapGateFree([&] { return fix.engine().enableInputChannels(2); });
    REQUIRE(on.success);
    REQUIRE(on.inputDeviceName == "Fake Interface");
}

// ── Input open failure degrades to output-only ──────────────────────────────

TEST_CASE("Swap: input-side open failure keeps the output and flags the input",
          "[SwapChar][inputs]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    sys->device("Fake Microphone")->failInputOpen = true;

    auto r = fix.engine().switchDevice("Fake Speakers", 0, 0, false,
                                       "Fake Microphone");
    REQUIRE(r.success);           // output survived
    REQUIRE(r.inputUnavailable);  // mic reported unavailable, not silent
    REQUIRE(fix.engine().currentDevice().name == "Fake Speakers");
}

// ── Driver switch ───────────────────────────────────────────────────────────

TEST_CASE("Swap: driver switch lands on the target driver's default device",
          "[SwapChar][driver]") {
    auto sys = makeSimpleSystem();
    auto second = std::make_shared<FakeDeviceSpec>();
    second->name = "Other Card";
    sys->types.push_back({ "OtherDriver", { second }, 0 });

    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().currentDriver() == "FakeDriver");

    auto r = fix.engine().switchDriver("OtherDriver");
    REQUIRE(r.success);
    REQUIRE(fix.engine().currentDriver() == "OtherDriver");
    REQUIRE(fix.engine().currentDevice().name == "Other Card");
}

// ── Reopen / recovery ───────────────────────────────────────────────────────

TEST_CASE("Swap: /clockwork/devices/reopen recovers through the factory seam",
          "[SwapChar][recovery]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    // The recovery worker tears the manager down and rebuilds it via
    // makeDeviceManager() — with the factory seam that means fresh fakes,
    // exactly like recovery after wake rebuilds against real hardware.
    OscReply reply;
    fix.send(osc_test::message("/clockwork/devices/reopen"));
    REQUIRE(fix.waitForReply("/clockwork/devices/reopen.done", reply, 10000));

    auto args = reply.parsed();
    REQUIRE(args.argCount() >= 1);
    REQUIRE(args.argInt(0) == 1);   // success

    // Engine is live on a (re-created) fake device afterwards.
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

TEST_CASE("Swap: a reopen lands back on the user's device, not on the system default "
          "it passes through", "[SwapChar][recovery]") {
    // The fresh device manager a reopen builds opens the default first. Staying
    // there would turn a device fault into a switch the user never asked for.
    auto sys = makeSimpleSystem();                       // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);

    fix.send(osc_test::message(CLOCKWORK_SYS("devices/reopen")));
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.reply"), reply));
    CHECK(reply.parsed().argInt(0) == 1);
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), reply, 10000));
    INFO(fix.debugMessagesDump());
    CHECK(reply.parsed().argInt(0) == 1);
    CHECK(reply.parsed().argString(1) == "Fake Interface");
    CHECK(fix.engine().currentDevice().name == "Fake Interface");
    CHECK(fix.engine().preferredOutputDevice() == "Fake Interface");

    fix.send(osc_test::message("/dummy/ping"));
    CHECK(fix.waitForReply("/dummy/pong", reply));
}

// ── Device-mutation phase ───────────────────────────────────────────────────


#ifdef __APPLE__
// A wireless link negotiates its own rate (AirPlay's is 44.1k). Leaving it
// for a wired device goes back to the rate the session had before it — the
// user's, not the link's — unless the switch asks for one. macOS's: a
// CoreAudio wireless default is opened through the default device.
TEST_CASE("Swap: leaving AirPlay goes back to the rate the session had before "
          "it, unless a rate is asked for", "[Swap]") {
    auto sys = std::make_shared<FakeSystem>();
    sys->types.push_back({ "FakeDriver", {}, 0 });
    addOutput(*sys, "Fake Speakers", 2);
    auto airplay = addOutput(*sys, "Fake AirPlay", 2);
    airplay->sampleRates = { 44100.0 };
    airplay->kind = "airp";
    airplay->wireless = true;
    auto bluetooth = addOutput(*sys, "Fake Bluetooth", 2);
    bluetooth->kind = "blue";
    bluetooth->wireless = true;
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));    // at 48k
    auto rate = [&] { return static_cast<int>(fix.engine().currentDevice().activeSampleRate); };

    REQUIRE(switchWhenFree(fix, "Fake AirPlay").success);
    CHECK(rate() == 44100);
    REQUIRE(switchWhenFree(fix, "Fake Bluetooth").success);
    CHECK(rate() == 44100);                 // wireless to wireless: the link's rate stays
    REQUIRE(switchWhenFree(fix, "Fake Speakers").success);
    INFO(fix.debugMessagesDump());
    CHECK(rate() == 48000);                 // back to the session's own

    REQUIRE(switchWhenFree(fix, "Fake AirPlay").success);
    REQUIRE(switchWhenFree(fix, "Fake Speakers", 44100.0).success);
    CHECK(rate() == 44100);                 // a rate asked for wins
}
#endif
