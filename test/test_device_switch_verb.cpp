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
#include <future>
#include <memory>
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

// Everything the engine has sent so far has arrived: this reply comes back
// through the same ring, after it.
void flushReplies(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r, 30000));
}

int rebuilds(EngineFixture& fix) {
    int n = 0;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("setup")) ++n;
    return n;
}

// Sonic Pi's "Enable audio inputs": 0 off, -1 back on; the engine's answer.
osc_test::ParsedReply enableInputs(EngineFixture& fix, int channels) {
    fix.send(osc_test::message(CLOCKWORK_SYS("inputs/enable"), channels));
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("inputs/enable.reply"), reply, 10000));
    return reply.parsed();
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

TEST_CASE("SwitchVerb: rapid picks collapse to the last, which alone ends in "
          "switch.done", "[SwitchVerb]") {
    // The wait exists so a user moving through a menu causes one switch, not
    // one per item. The pick a later one replaced never runs, so the only
    // switch.done is the last pick's: a client (Sonic Pi) that pairs its
    // pending pick with the next switch.done must not be handed another.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    // Both picks wait behind this, so the second always replaces the first.
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    fix.engine().postDeviceTask([released] { released.wait(); });

    OscReply reply;
    fix.send(switchTo("Fake Microphone", ""));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    fix.send(switchTo("Fake Interface", ""));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    release.set_value();

    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    flushReplies(fix);

    const auto dones = switchDones(fix);
    REQUIRE(dones.size() == 1);
    CHECK(dones[0].argInt(0) == 1);
    CHECK(dones[0].argString(1) == "Fake Interface");
    CHECK(dones[0].argString(3) == "Fake Interface");
}

TEST_CASE("SwitchVerb: a pick made while another is switching is not lost",
          "[SwitchVerb]") {
    // The debounce worker had taken the first pick and was switching to it
    // when the second came. The second found the worker running and left it
    // to the worker, which then finished the first and stopped: the second
    // was never switched to, and never answered, until the next pick.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    auto gate = fix.engine().testHoldSwapGate();   // the first switch waits on it
    OscReply reply;
    fix.send(switchTo("Fake Interface", ""));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    const bool taken = fix.pollUntil([&] {
        for (const auto& line : fix.debugMessages())
            if (line.find("debounced switch: out='Fake Interface'") != std::string::npos) return true;
        return false;
    }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(taken);

    fix.send(switchTo("Fake Speakers", ""));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    // The reply is sent before the pick is handed to the worker; a verb behind
    // it on the same control thread is answered after.
    flushReplies(fix);
    gate.unlock();

    const bool done = fix.pollUntil([&] {
        for (const auto& d : switchDones(fix))
            if (d.argString(1) == "Fake Speakers") return true;
        return false;
    }, 10000);
    REQUIRE(done);
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
}

TEST_CASE("SwitchVerb: a menu label sent as the input, such as '-- None --', is "
          "refused by name and the engine plays on where it was", "[SwitchVerb]") {
    // Seen from the field: the GUI's display string for "no input" leaked onto
    // the wire. Refused before anything is torn down, with the name in the
    // error, rather than failing halfway through a swap.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    for (const char* input : { "-- None --", "Phantom Mic" }) {
        INFO("input: " << input);
        fix.send(switchTo("", input));
        OscReply reply;
        REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
        CHECK(reply.parsed().argInt(0) == 1);

        const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
        INFO(fix.debugMessagesDump());
        REQUIRE(done);
        flushReplies(fix);
        const auto dones = switchDones(fix);
        CHECK(dones.size() == 1);
        const auto& d = dones.front();
        CHECK(d.argInt(0) == 0);
        CHECK(d.argString(2) == input);
        CHECK(d.argString(5).find(input) != std::string::npos);
        CHECK(d.argString(5).find("input") != std::string::npos);
        CHECK(rebuilds(fix) == 0);
        CHECK(fix.engine().currentDevice().name == "Fake Speakers");
        CHECK(fix.waitForBlocks(20));
        fix.clearReplies();
    }
}

TEST_CASE("SwitchVerb: an output name longer than any device's is refused in "
          "switch.done, and the engine plays on where it was", "[SwitchVerb]") {
    // A name is whatever a client sends. Refusing this one built a switch.done
    // carrying it twice (asked for, and in the error) in a fixed 2 KB buffer.
    // That threw on the device lane, where nothing catches: std::terminate,
    // and any host the engine is embedded in with it. Then, built to fit, the
    // switch.done was bigger than an egress frame and was dropped unsaid.
    // Now no device lane is troubled with it: refused at once, echoed cut.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    const std::string name = "Phantom " + std::string(3000, 'x');
    fix.send(switchTo(name.c_str(), ""));
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.reply"), reply));
    CHECK(reply.parsed().argInt(0) == 1);

    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    flushReplies(fix);
    const auto dones = switchDones(fix);
    CHECK(dones.size() == 1);
    const auto& d = dones.front();
    CHECK(d.argInt(0) == 0);
    CHECK(d.argString(1).rfind("Phantom xxxx", 0) == 0);
    CHECK(d.argString(1).size() < 80);
    CHECK(d.argString(5).find("that long") != std::string::npos);
    CHECK(rebuilds(fix) == 0);
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    CHECK(fix.waitForBlocks(20));
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

TEST_CASE("InputsVerb: turning inputs on for an eight-input interface opens all eight, "
          "and turning them back on after off opens all eight again", "[SwitchVerb]") {
    // Sonic Pi boots with inputs off (-i 0), which says nothing about how many
    // the user wants once they turn them on. Capping at stereo left an
    // eight-input interface at two.
    auto sys = std::make_shared<fake_audio::FakeSystem>();
    auto box = std::make_shared<fake_audio::FakeDeviceSpec>();
    box->name = "Fake 8in Interface";
    box->maxOutputChannels = 2;
    box->maxInputChannels  = 8;
    sys->types.push_back({ "FakeDriver", { box }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake 8in Interface"));
    subscribe(fix);

    fix.send(switchTo("", "Fake 8in Interface"));
    const bool done = fix.pollUntil([&] { return !switchDones(fix).empty(); }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(done);
    const auto d = switchDones(fix).front();
    CHECK(d.argInt(0) == 1);
    CHECK(d.argString(4) == "Fake 8in Interface");
    OscReply rebuilt;                      // the engine rebuilt with input channels
    CHECK(fix.waitForReply(CLOCKWORK_SYS("setup"), rebuilt, 10000));
    CHECK(fix.engine().currentDevice().activeInputChannels == 8);

    auto off = enableInputs(fix, 0);
    CHECK(off.argInt(0) == 1);
    CHECK(off.argInt(1) == 0);
    CHECK(fix.engine().currentDevice().activeInputChannels == 0);

    auto on = enableInputs(fix, -1);
    CHECK(on.argInt(0) == 1);
    CHECK(on.argInt(1) == -1);
    CHECK(fix.engine().currentDevice().activeInputChannels == 8);
}
