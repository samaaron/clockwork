// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_boot_input_pairing.cpp — the -H boot must never activate an input
 * device it wasn't explicitly given.
 *
 * With inputs enabled and a blank input name, JUCE's initialise fills the
 * name with the type default (insertDefaultDeviceNames). On macOS that
 * implicit pick opened the default mic through JUCE's Combiner — the
 * intermittent RC6 boot SIGSEGV / distorted-audio reports. The engine's
 * contract: input pairing is always an explicit engine decision (same-device
 * full duplex at open, or aggregate promotion after it).
 *
 * The explicit decision has its own failures: the user's saved microphone has
 * to be the one paired, an unplugged one must not cost the boot its output,
 * and neither must one saved under another driver's name for the same box.
 */
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "OscTestUtils.h"
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

TEST_CASE("Boot: -H output with inputs enabled activates no implicit input",
          "[DeviceSelection][boot]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.numInputChannels = 2;
    EngineFixture fix(cfg);

    auto cur = fix.engine().currentDevice();
    REQUIRE(cur.name == "Fake Speakers");
    // Uniform contract on every platform: a -H boot never opens an
    // implicit input. Input pairing belongs to the explicit paths —
    // aggregate promotion on macOS, the preferred-input pairing
    // block elsewhere.
    REQUIRE(cur.activeInputChannels == 0);

    OscReply reply;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

TEST_CASE("Boot: -H full-duplex device with matching input pref keeps its input",
          "[DeviceSelection][boot]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.numInputChannels = 2;
    cfg.inputDevice = "Fake Interface";
    EngineFixture fix(cfg);

    auto cur = fix.engine().currentDevice();
    REQUIRE(cur.name == "Fake Interface");
    REQUIRE(cur.activeInputChannels > 0);
}

// ── A saved microphone, paired with the output as the engine comes up ───────
// The driver opens the output and the input as one device (CoreAudio on an
// aggregate of the two), and the boot pairs the saved one itself; otherwise a
// GUI would find its choice not honoured on every launch and correct it with
// a needless rebuild.

namespace {

// The driver the engine says it is on, as the GUI's driver menu reads it.
std::string driverInUse(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("drivers/list")));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("drivers/list.reply"), r, 10000));
    return r.parsed().argString(0);
}

std::shared_ptr<fake_audio::FakeDeviceSpec> device(const std::string& name, int outs, int ins) {
    auto d = std::make_shared<fake_audio::FakeDeviceSpec>();
    d->name = name;
    d->maxOutputChannels = outs;
    d->maxInputChannels  = ins;
    return d;
}

}  // namespace

TEST_CASE("Boot: a saved microphone is recorded from as the engine comes up, and a "
          "saved one that is unplugged leaves the output playing",
          "[DeviceSelection][boot]") {
    auto withSavedMic = [](std::shared_ptr<fake_audio::FakeSystem> sys) {
        auto cfg = fakeEngineConfig(std::move(sys), "Fake Speakers");
        cfg.numInputChannels = 2;
        cfg.inputDevice = "Fake Microphone";
        return cfg;
    };
    {
        auto sys = makeSimpleSystem();
        EngineFixture fix(withSavedMic(sys));
        const auto cur = fix.engine().currentDevice();
        INFO(fix.debugMessagesDump());
        CHECK(cur.name == "Fake Speakers");
        CHECK(cur.inputDeviceName == "Fake Microphone");
        CHECK(cur.activeInputChannels == 2);
        fix.send(osc_test::message(CLOCKWORK_SYS("devices/report")));
        OscReply inputs;
        REQUIRE(fix.waitForReply(CLOCKWORK_SYS("input-devices"), inputs, 10000));
        CHECK(inputs.parsed().argString(0) == "Fake Microphone");
    }
    {
        auto sys = makeSimpleSystem();
        sys->device("Fake Microphone")->hidden = true;   // unplugged since it was saved
        EngineFixture fix(withSavedMic(sys));
        const auto cur = fix.engine().currentDevice();
        CHECK(cur.name == "Fake Speakers");
        CHECK(cur.activeInputChannels == 0);
        CHECK(fix.waitForBlocks(20));
    }
}

TEST_CASE("Boot: a saved microphone known by another driver's name doesn't cost the "
          "output on the driver in use", "[DeviceSelection][boot]") {
    // One MOTU box as Windows lists it: "MOTU Pro Audio" under ASIO, where
    // the input menu mirrors the output, and "Speakers (MOTU)" and "In 1-2
    // (MOTU)" under Windows Audio. The microphone was saved while on ASIO.
    // Tried under Windows Audio, it was "No such device" with the output
    // already open, and the engine came up with no device at all.
    auto machine = [] {
        auto sys = std::make_shared<fake_audio::FakeSystem>();
        sys->types.push_back({ "Windows Audio", { device("Speakers (MOTU)", 2, 0),
                                                  device("In 1-2 (MOTU)", 0, 2) }, 0 });
        sys->types.push_back({ "ASIO", { device("MOTU Pro Audio", 2, 2) }, 0 });
        return sys;
    };
    auto boot = [](std::shared_ptr<fake_audio::FakeSystem> sys, const std::string& savedMic) {
        auto cfg = fakeEngineConfig(std::move(sys), "Speakers (MOTU)");
        cfg.audioDriver = "Windows Audio";
        cfg.numInputChannels = 2;
        cfg.inputDevice = savedMic;
        return cfg;
    };

    // How often a boot with no saved microphone opens the output.
    int cleanBootOpens = 0;
    {
        auto sys = machine();
        EngineFixture fix(boot(sys, ""));
        cleanBootOpens = sys->device("Speakers (MOTU)")->opens.load();
    }

    auto sys = machine();
    EngineFixture fix(boot(sys, "MOTU Pro Audio"));
    INFO(fix.debugMessagesDump());
    CHECK(driverInUse(fix) == "Windows Audio");
    CHECK(fix.engine().currentDevice().name == "Speakers (MOTU)");
    CHECK(fix.engine().currentDevice().activeInputChannels == 0);
    CHECK(sys->device("MOTU Pro Audio")->opens.load() == 0);
    // Not tried at all: a pairing that fails takes the output down with it,
    // and the output is opened again — a dropout at every launch.
    CHECK(sys->device("Speakers (MOTU)")->opens.load() == cleanBootOpens);
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("Boot: a saved microphone its driver won't pair with the output isn't tried",
          "[DeviceSelection][boot]") {
    // AirPods' microphone, saved while the AirPods played. CoreAudio can't
    // put a wireless device in an aggregate (its codec drops the pair to
    // 16 kHz, #3555): tried, the pair is refused with the output already
    // closed, and the output is opened again — a dropout at every launch.
    auto machine = [] {
        auto sys = makeSimpleSystem();
        auto airpods = device("Fake AirPods", 2, 1);
        airpods->wireless = true;
        sys->types[0].devices.push_back(airpods);
        return sys;
    };
    auto boot = [](std::shared_ptr<fake_audio::FakeSystem> sys, const std::string& savedMic) {
        auto cfg = fakeEngineConfig(std::move(sys), "Fake Speakers");
        cfg.numInputChannels = 2;
        cfg.inputDevice = savedMic;
        return cfg;
    };

    int cleanBootOpens = 0;
    {
        auto sys = machine();
        EngineFixture fix(boot(sys, ""));
        cleanBootOpens = sys->device("Fake Speakers")->opens.load();
    }

    auto sys = machine();
    EngineFixture fix(boot(sys, "Fake AirPods"));
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    CHECK(fix.engine().currentDevice().activeInputChannels == 0);
    CHECK(sys->device("Fake AirPods")->opens.load() == 0);
    CHECK(sys->device("Fake Speakers")->opens.load() == cleanBootOpens);
    CHECK(fix.waitForBlocks(20));
}
