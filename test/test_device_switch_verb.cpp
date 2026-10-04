// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_switch_verb.cpp — /clockwork/devices/switch, as a client sends it.
 *
 * The contract: the verb is acknowledged at once (switch.reply), the work runs
 * off the control pass, and it ends in exactly one switch.done broadcast saying
 * what was asked for, where the engine ended up, and why if it failed. A client
 * records the outcome from switch.done — Sonic Pi saves its output choice then,
 * and only then.
 *
 * Seen with Sonic Pi 5.0.0 (2026-10-02): picking "OS Default" never produced a
 * switch.done, so the choice was never saved, and every later launch booted
 * pinned to the device the user had picked before it.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <string>
#include <vector>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

osc_test::Packet switchTo(const char* output, const char* input) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << output << 0.0f << static_cast<osc::int32>(0) << input;
    return b.end();
}

// Every switch.done the engine has broadcast so far, oldest first.
std::vector<osc_test::ParsedReply> switchDones(EngineFixture& fix) {
    std::vector<osc_test::ParsedReply> out;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("devices/switch.done")) out.push_back(r.parsed());
    return out;
}

// Booted, and subscribed to the engine's broadcasts as a GUI is.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

}  // namespace

TEST_CASE("SwitchVerb: picking the system default ends in switch.done, naming "
          "where the engine landed", "[SwitchVerb]") {
    auto sys = makeSimpleSystem();            // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);

    fix.send(switchTo("__system__", ""));
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    CHECK(reply.parsed().argInt(0) == 1);     // accepted

    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    const auto dones = switchDones(fix);
    CHECK(dones.size() == 1);
    const auto& d = dones.front();
    CHECK(d.argInt(0) == 1);                          // success
    CHECK(d.argString(1) == "__system__");            // what was asked for
    CHECK(d.argString(3) == "Fake Speakers");         // where it landed
    CHECK(d.argString(5).empty());                    // no error
}

TEST_CASE("SwitchVerb: turning inputs off ends in switch.done", "[SwitchVerb]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    fix.send(switchTo("", "__none__"));
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    CHECK(reply.parsed().argInt(0) == 1);

    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    const auto dones = switchDones(fix);
    CHECK(dones.size() == 1);
    CHECK(dones.front().argInt(0) == 1);
    CHECK(dones.front().argString(2) == "__none__");
    CHECK(dones.front().argString(3) == "Fake Speakers");
}

TEST_CASE("SwitchVerb: picking a device ends in switch.done", "[SwitchVerb]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    fix.send(switchTo("Fake Interface", ""));      // the GUI's shape for an output pick
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    CHECK(reply.parsed().argInt(0) == 1);

    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    const auto dones = switchDones(fix);
    CHECK(dones.size() == 1);
    CHECK(dones.front().argInt(0) == 1);
    CHECK(dones.front().argString(1) == "Fake Interface");
    CHECK(dones.front().argString(3) == "Fake Interface");
}

TEST_CASE("SwitchVerb: turning inputs off while following the system default "
          "keeps following it", "[SwitchVerb]") {
    // Turning the mic off is not choosing an output. It used to lock the
    // engine to whatever it was playing on (manual mode), and from then on a
    // change of the system default was ignored.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());
    subscribe(fix);

    fix.send(switchTo("", "__none__"));
    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    CHECK(fix.engine().deviceMode().empty());
}

TEST_CASE("InputsVerb: turning inputs off while following the system default "
          "keeps following it", "[SwitchVerb]") {
    // Sonic Pi's "Enable audio inputs" menu item sends /clockwork/inputs/enable.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fix.engine().setDeviceMode("system").empty());

    fix.send(osc_test::message(CLOCKWORK_SYS("inputs/enable"), 0));
    OscReply reply;
    const bool replied = fix.waitForReply(CLOCKWORK_SYS("inputs/enable.reply"), reply, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(replied);
    CHECK(reply.parsed().argInt(0) == 1);
    CHECK(fix.engine().deviceMode().empty());
}
