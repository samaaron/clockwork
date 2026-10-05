// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_names.cpp — a device found by the name the user knows it by.
 *
 * Two identical interfaces are listed by the device layer as "USB Audio (1)"
 * and "USB Audio (2)": it numbers duplicate names to tell them apart, while the
 * OS, a saved preference and the user know only "USB Audio". The engine has to
 * land a plain name on the first of them, keep a numbered one as it is, and
 * follow the user's device when it comes back under a number. And it must never
 * take the numbering rule for a looser one: "USB Audio Pro" is a different box
 * that merely starts with the same words, and "Speakers (Main)" carries a
 * parenthesis of its own. Too strict, and the user's device is refused with "No
 * such device"; too loose, and sound goes to a device they never chose.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <memory>
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::FakeDeviceSpec;
using fake_audio::FakeSystem;

namespace {

// An output-only device, plugged in or not yet.
std::shared_ptr<FakeDeviceSpec> output(const std::string& name, bool unplugged = false) {
    auto d = std::make_shared<FakeDeviceSpec>();
    d->name = name;
    d->maxInputChannels = 0;
    d->hidden = unplugged;
    return d;
}

// Booted, and subscribed to the engine's broadcasts as a GUI is.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

// The user picks an output from the menu; what the engine says it did.
osc_test::ParsedReply pick(EngineFixture& fix, const char* name) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << name << 0.0f << static_cast<osc::int32>(0) << "";
    fix.send(b.end());
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), done, 10000));
    return done.parsed();
}

bool playingOn(EngineFixture& fix, const std::string& name, int timeoutMs = 10000) {
    return fix.pollUntil([&] { return fix.engine().currentDevice().name == name; },
                         timeoutMs);
}

// Whether the engine has told its clients it is playing on `name`.
bool reported(EngineFixture& fix, const std::string& name) {
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("devices") && r.parsed().argString(1) == name)
            return true;
    return false;
}

}  // namespace

TEST_CASE("DeviceNames: a device asked for by its plain name opens the first of two "
          "identical interfaces, and a different device that only shares the start "
          "of its name is never opened", "[DeviceNames]") {
    auto sys = std::make_shared<FakeSystem>();
    auto first  = output("USB Audio (1)", true);
    auto second = output("USB Audio (2)", true);
    auto pro    = output("USB Audio Pro");
    auto mains  = output("Speakers (Main)");
    sys->types.push_back({ "FakeDriver",
                           { output("Fake Speakers"), first, second, pro, mains }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    // Neither interface is plugged in: what shares their name is not them.
    auto done = pick(fix, "USB Audio");
    CHECK(done.argInt(0) == 0);
    CHECK(done.argString(5).find("USB Audio") != std::string::npos);
    CHECK(pro->opens.load() == 0);
    done = pick(fix, "Speakers");
    CHECK(done.argInt(0) == 0);
    CHECK(mains->opens.load() == 0);
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    CHECK(fix.waitForBlocks(20));

    {   // both plugged in
        auto hold = fix.engine().testHoldSwapGate();
        first->hidden  = false;
        second->hidden = false;
    }
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());

    done = pick(fix, "USB Audio");
    INFO(fix.debugMessagesDump());
    CHECK(done.argInt(0) == 1);
    CHECK(done.argString(3) == "USB Audio (1)");
    // A numbered name is the device it names, not the first of the pair.
    done = pick(fix, "USB Audio (2)");
    CHECK(done.argInt(0) == 1);
    CHECK(done.argString(3) == "USB Audio (2)");
    CHECK(fix.engine().currentDevice().name == "USB Audio (2)");
    CHECK(pro->opens.load() == 0);
}

TEST_CASE("DeviceNames: the user's interface coming back as 'USB Audio (2)' is played "
          "on again, and a 'USB Audio Pro' plugged in meanwhile is not mistaken for it",
          "[DeviceNames]") {
    auto sys = std::make_shared<FakeSystem>();
    auto usb     = output("USB Audio");
    auto pro     = output("USB Audio Pro", true);
    auto renamed = output("USB Audio (2)", true);
    sys->types.push_back({ "FakeDriver", { output("Fake Speakers"), usb, pro, renamed }, 0 });
    auto cfg = fakeEngineConfig(sys, "USB Audio");
    cfg.watchdogRecoveryCooldownMs = 0;   // the reopen at the end follows one closely
    EngineFixture fix(cfg);
    subscribe(fix);
    REQUIRE(fix.engine().currentDevice().name == "USB Audio");

    usb->hidden = true;                   // unplugged: the default plays meanwhile
    REQUIRE(sys->reportListChanged());
    OscReply reopened;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), reopened, 10000));
    REQUIRE(reopened.parsed().argString(1) == "Fake Speakers");

    pro->hidden = false;                  // a different box, sharing the start of its name
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    CHECK(pro->opens.load() == 0);

    // Plugged back in where the device layer now numbers it.
    renamed->hidden = false;
    REQUIRE(sys->reportListChanged());
    const bool back = playingOn(fix, "USB Audio (2)");
    INFO(fix.debugMessagesDump());
    REQUIRE(back);
    CHECK(fix.pollUntil([&] { return reported(fix, "USB Audio (2)"); }, 10000));

    // A reopen goes back to it too, rather than staying on the default the
    // fresh device manager opens first.
    fix.send(osc_test::message(CLOCKWORK_SYS("devices/reopen")));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), reopened, 10000));
    CHECK(reopened.parsed().argInt(0) == 1);
    CHECK(reopened.parsed().argString(1) == "USB Audio (2)");
    CHECK(pro->opens.load() == 0);
}
