// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_intent.cpp — the engine opens what it is asked to play on,
 * directly.
 *
 * Every open of a device is heard: a click, a dropout, a moment of silence
 * on whatever else plays through it, and on CoreAudio an aggregate made and
 * pulled down again. JUCE's device manager opened the system defaults first
 * and then whatever was wanted, so the engine did too — a recovery opened the
 * default and then the user's device; a boot opened the output, then again
 * with the user's microphone, then again at the session's rate. These pin
 * the opens to what was asked for: the user's devices, once.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"

#include <string>

using fake_audio::FakeSystem;
using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

int opens(FakeSystem& sys, const std::string& name) { return sys.at(name).opens.load(); }

void resetOpens(FakeSystem& sys) {
    for (auto& t : sys.types)
        for (auto& d : t.devices) d->opens.store(0);
}

}  // namespace

TEST_CASE("DeviceIntent: a boot opens the user's output, microphone, rate and buffer at once",
          "[DeviceIntent][boot]") {
    // The fakes run at 48 kHz until asked otherwise: a session at 44.1 kHz is
    // a rate the device must be opened at, not opened and then changed to.
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.sampleRate       = 44100;
    cfg.numInputChannels = 2;
    cfg.inputDevice      = "Fake Microphone";
    EngineFixture fix(cfg);
    INFO(fix.debugMessagesDump());

    const auto cur = fix.engine().currentDevice();
    CHECK(cur.name == "Fake Speakers");
    CHECK(cur.inputDeviceName == "Fake Microphone");
    CHECK(static_cast<int>(cur.activeSampleRate) == 44100);
    CHECK(cur.activeBufferSize == 128);
    CHECK(opens(*sys, "Fake Speakers") == 1);
    CHECK(opens(*sys, "Fake Microphone") == 0);   // opened as the pair's input, under the speakers
    CHECK(fix.waitForBlocks(10));
}

TEST_CASE("DeviceIntent: a recovery opens the user's devices, never the system default on the way",
          "[DeviceIntent][recovery]") {
    // The default is the speakers; the user plays and records on the
    // interface. A recovery makes a fresh device session and opens what the
    // user chose on it: the speakers are not opened at all.
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.numInputChannels           = 2;
    cfg.inputDevice                = "Fake Interface";
    cfg.watchdogRecoveryCooldownMs = 0;
    cfg.hostDrivesControl          = true;   // hear the recovery's own account
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().inputDeviceName == "Fake Interface");
    resetOpens(*sys);

    fix.send(osc_test::message(CLOCKWORK_SYS("devices/reopen")));
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), done, 20000));
    INFO(fix.debugMessagesDump());
    CHECK(done.parsed().argInt(0) == 1);
    CHECK(done.parsed().argString(1) == "Fake Interface");

    const auto cur = fix.engine().currentDevice();
    CHECK(cur.name == "Fake Interface");
    CHECK(cur.inputDeviceName == "Fake Interface");
    CHECK(opens(*sys, "Fake Speakers") == 0);
    CHECK(opens(*sys, "Fake Interface") == 1);

    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    OscReply pong;
    REQUIRE(fix.waitForReply("/dummy/pong", pong));
}

TEST_CASE("DeviceIntent: a rate change opens no input the user turned off",
          "[DeviceIntent]") {
    // The interface plays and records, and the user booted with inputs off:
    // that it HAS inputs is not that they are wanted. A rate change reopens
    // the output, and only the output.
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Interface");   // inputs off
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeInputChannels == 0);

    const auto r = fix.engine().switchDevice("", 44100);
    INFO(fix.debugMessagesDump());
    REQUIRE(r.success);
    const auto cur = fix.engine().currentDevice();
    CHECK(static_cast<int>(cur.activeSampleRate) == 44100);
    CHECK(cur.activeInputChannels == 0);
    CHECK(cur.inputDeviceName.empty());
}
