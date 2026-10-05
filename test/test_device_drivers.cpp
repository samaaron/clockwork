// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_drivers.cpp — the driver a device is played through.
 *
 * Windows lists one piece of hardware under several drivers at once: the same
 * "Speakers (Realtek)" under Windows Audio and DirectSound, and a MOTU box as
 * "MOTU Pro Audio" under ASIO but "Speakers (MOTU)" under Windows Audio. A
 * device pick therefore means a device on the driver the user is on; another
 * driver is chosen by choosing the driver, and the device follows. ASIO has no
 * default device at all — opening one to find out can hang in the driver — so
 * choosing it opens nothing until its device is picked, and a boot that saved
 * ASIO without a device stays on the platform's driver.
 *
 * Each of these was a bug: a DirectSound session moved to Windows Audio by a
 * pick of its own device, an ASIO device pick flipping the driver behind the
 * user's back, a recovery between the two steps eating the ASIO choice, and a
 * saved device booting under whichever driver had the shortest name.
 *
 * The fake drivers carry Windows' names. Nothing here is Windows-only: the
 * engine treats these drivers by name on every platform.
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

std::shared_ptr<FakeDeviceSpec> device(const std::string& name, int outs, int ins) {
    auto d = std::make_shared<FakeDeviceSpec>();
    d->name = name;
    d->maxOutputChannels = outs;
    d->maxInputChannels  = ins;
    return d;
}

ClockworkEngine::Config bootOn(std::shared_ptr<FakeSystem> sys, const std::string& driver,
                               const std::string& output) {
    auto cfg = fakeEngineConfig(std::move(sys), output);
    cfg.audioDriver = driver;   // --audio-driver, as the saved preference boots it
    return cfg;
}

// A remote desktop's speakers and headphones on Windows Audio, and an ASIO box.
std::shared_ptr<FakeSystem> remoteDesktopWithAsio() {
    auto sys = std::make_shared<FakeSystem>();
    sys->types.push_back({ "Windows Audio", { device("Remote Audio", 2, 0),
                                              device("Remote Headphones", 2, 0) }, 0 });
    sys->types.push_back({ "ASIO", { device("MOTU Pro Audio", 2, 2) }, 0 });
    return sys;
}

// Booted, and subscribed to the engine's broadcasts as a GUI is.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

// The driver the engine says it is on, as the GUI's driver menu reads it.
std::string driverInUse(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("drivers/list")));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("drivers/list.reply"), r, 10000));
    return r.parsed().argString(0);
}

// The user picks a driver from the menu.
osc_test::ParsedReply pickDriver(EngineFixture& fix, const char* driver) {
    fix.send(osc_test::message(CLOCKWORK_SYS("drivers/switch"), driver));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("drivers/switch.reply"), r, 10000));
    return r.parsed();
}

// The user picks an output; what the engine says it did.
osc_test::ParsedReply pick(EngineFixture& fix, const char* output, float rate = 0.0f) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << output << rate << static_cast<osc::int32>(0) << "";
    fix.send(b.end());
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), done, 10000));
    return done.parsed();
}

bool mentions(const std::string& text, const char* what) {
    return text.find(what) != std::string::npos;
}

}  // namespace

TEST_CASE("Drivers: a device picked on the driver in use stays on that driver when "
          "another driver lists it too, and a device only another driver has is "
          "refused, naming both", "[Drivers]") {
    auto sys = std::make_shared<FakeSystem>();
    auto focusrite = device("Focusrite USB ASIO", 2, 2);
    sys->types.push_back({ "Windows Audio", { device("Speakers (Realtek)", 2, 0) }, 0 });
    sys->types.push_back({ "DirectSound", { device("Speakers (Realtek)", 2, 0),
                                            device("Primary Sound Driver", 2, 0) }, 0 });
    sys->types.push_back({ "ASIO", { focusrite }, 0 });
    EngineFixture fix(bootOn(sys, "DirectSound", "Speakers (Realtek)"));
    subscribe(fix);
    REQUIRE(driverInUse(fix) == "DirectSound");

    // With a rate, so the pick is a real switch rather than "already there".
    auto done = pick(fix, "Speakers (Realtek)", 44100.0f);
    INFO(fix.debugMessagesDump());
    CHECK(done.argInt(0) == 1);
    CHECK(driverInUse(fix) == "DirectSound");

    done = pick(fix, "Focusrite USB ASIO");
    CHECK(done.argInt(0) == 0);
    CHECK(mentions(done.argString(5), "Focusrite USB ASIO"));
    CHECK(mentions(done.argString(5), "DirectSound"));
    CHECK(focusrite->opens.load() == 0);
    CHECK(driverInUse(fix) == "DirectSound");
    CHECK(fix.waitForBlocks(20));
}

TEST_CASE("Drivers: picking ASIO and then its device lands on that device with its "
          "inputs, even when the engine recovers in between", "[Drivers]") {
    auto sys = remoteDesktopWithAsio();
    auto motu = sys->device("MOTU Pro Audio");
    EngineFixture fix(bootOn(sys, "Windows Audio", "Remote Audio"));
    subscribe(fix);

    // ASIO has no default device: the choice is kept, and nothing opens until
    // the user picks one of its devices.
    CHECK(pickDriver(fix, "ASIO").argInt(0) == 1);
    CHECK(fix.waitForBlocks(20));
    CHECK(fix.engine().currentDevice().name == "Remote Audio");
    CHECK(motu->opens.load() == 0);

    // The engine reopens its device meanwhile (a device fault, or Reset).
    fix.send(osc_test::message(CLOCKWORK_SYS("devices/reopen")));
    OscReply reopened;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), reopened, 10000));
    CHECK(reopened.parsed().argInt(0) == 1);
    CHECK(reopened.parsed().argString(1) == "Remote Audio");
    fix.clearReplies();                   // the reopen's own rebuild has been said

    const auto done = pick(fix, "MOTU Pro Audio");
    INFO(fix.debugMessagesDump());
    CHECK(done.argInt(0) == 1);
    CHECK(done.argString(3) == "MOTU Pro Audio");
    CHECK(done.argString(4) == "MOTU Pro Audio");   // an ASIO device is its own input
    OscReply rebuilt;
    CHECK(fix.waitForReply(CLOCKWORK_SYS("setup"), rebuilt, 10000));
    CHECK(driverInUse(fix) == "ASIO");
    CHECK(fix.engine().currentDevice().activeInputChannels == 2);
}

TEST_CASE("Drivers: after picking ASIO, a device only a third driver has is refused "
          "naming ASIO, and picking a device of the driver in use stays there and "
          "ends the ASIO pick", "[Drivers]") {
    auto sys = remoteDesktopWithAsio();
    sys->types.push_back({ "DirectSound", { device("Primary Sound Driver", 2, 0) }, 0 });
    auto motu = sys->device("MOTU Pro Audio");
    EngineFixture fix(bootOn(sys, "Windows Audio", "Remote Audio"));
    subscribe(fix);
    REQUIRE(pickDriver(fix, "ASIO").argInt(0) == 1);

    // Neither ASIO's nor the driver in use's: the refusal names the driver the
    // user chose. (A name no driver lists at all is refused by that name alone.)
    auto done = pick(fix, "Primary Sound Driver");
    CHECK(done.argInt(0) == 0);
    CHECK(mentions(done.argString(5), "ASIO"));

    // A device of the driver in use: the user has walked away from ASIO.
    done = pick(fix, "Remote Headphones");
    INFO(fix.debugMessagesDump());
    CHECK(done.argInt(0) == 1);
    CHECK(done.argString(3) == "Remote Headphones");
    CHECK(driverInUse(fix) == "Windows Audio");

    // So an ASIO device is now another driver's, and refused as one.
    done = pick(fix, "MOTU Pro Audio");
    CHECK(done.argInt(0) == 0);
    CHECK(mentions(done.argString(5), "Windows Audio"));
    CHECK(motu->opens.load() == 0);
    CHECK(fix.engine().currentDevice().name == "Remote Headphones");
}

// ── Boot ────────────────────────────────────────────────────────────────────

TEST_CASE("Drivers: a saved driver is booted on by its exact name, and ASIO without a "
          "saved device is never opened at boot", "[Drivers][boot]") {
    auto sys = std::make_shared<FakeSystem>();
    auto motu = device("MOTU Pro Audio", 2, 2);
    sys->types.push_back({ "Windows Audio", { device("Speakers", 2, 0) }, 0 });
    sys->types.push_back({ "Windows Audio (Low Latency Mode)", { device("Speakers LL", 2, 0) }, 0 });
    sys->types.push_back({ "DirectSound", { device("Primary Sound Driver", 2, 0) }, 0 });
    sys->types.push_back({ "ASIO", { motu }, 0 });

    {   // ASIO saved, no device: the platform's driver plays instead
        EngineFixture fix(bootOn(sys, "ASIO", ""));
        CHECK(driverInUse(fix) == "Windows Audio");
        CHECK(motu->opens.load() == 0);
        CHECK(fix.waitForBlocks(20));
    }
    {   // a driver whose name another one's starts with
        EngineFixture fix(bootOn(sys, "Windows Audio (Low Latency Mode)", ""));
        CHECK(driverInUse(fix) == "Windows Audio (Low Latency Mode)");
        CHECK(fix.engine().currentDevice().name == "Speakers LL");
    }
    {   // ASIO with its device saved
        EngineFixture fix(bootOn(sys, "ASIO", "MOTU Pro Audio"));
        CHECK(driverInUse(fix) == "ASIO");
        CHECK(fix.engine().currentDevice().name == "MOTU Pro Audio");
    }
}

TEST_CASE("Drivers: a saved device boots on the saved driver even when another driver "
          "lists it under the same name, and one only another driver has still boots",
          "[Drivers][boot]") {
    auto sys = std::make_shared<FakeSystem>();
    sys->types.push_back({ "Windows Audio", { device("Speakers (X)", 2, 0) }, 0 });
    sys->types.push_back({ "DirectSound", { device("Speakers (X)", 2, 0),
                                            device("Primary Sound Driver", 2, 0) }, 0 });
    {
        EngineFixture fix(bootOn(sys, "Windows Audio", "Speakers (X)"));
        CHECK(driverInUse(fix) == "Windows Audio");
        CHECK(fix.engine().currentDevice().name == "Speakers (X)");
    }
    {   // a stale driver choice beside a good device choice
        EngineFixture fix(bootOn(sys, "Windows Audio", "Primary Sound Driver"));
        CHECK(driverInUse(fix) == "DirectSound");
        CHECK(fix.engine().currentDevice().name == "Primary Sound Driver");
    }
}
