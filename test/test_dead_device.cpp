// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_dead_device.cpp — a device that opens and starts but never delivers
 * a block (FakeDeviceSpec::failStart).
 *
 * Seen on macOS with Sonic Pi 5.0.0 (SuperSonic 0.71.0), twice:
 *   - a workshop laptop (2026-09-29): Bluetooth earbuds dropped, the engine
 *     followed the system default back to the built-in speakers, and the
 *     speakers never called back;
 *   - a gig (2026-10-02): the GUI's restore moved a session that was playing
 *     on the headphone jack onto the speakers, which never called back.
 * Both times the swap waited 5 s for a first block, logged "audio callbacks
 * not firing", and reported success anyway — so the GUI showed a device that
 * was producing nothing. A recovery onto such a device reported it
 * "restored" in the same way. A device that never ticks is a failed swap,
 * exactly as a device that refuses to open is: say so, and go back to the
 * device that was playing.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

TEST_CASE("DeadDevice: a switch to a device that never delivers audio fails, and "
          "the engine plays on the device it had", "[DeadDevice]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Interface").failStart = true;
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));

    auto r = fix.engine().switchDevice("Fake Interface", 0, 0, false, "__none__");
    INFO(fix.debugMessagesDump());
    CHECK_FALSE(r.success);
    // Said to the person who picked it, in their words: what they heard (or
    // didn't), and where the sound is now.
    CHECK(r.error == "No sound came out of Fake Interface, so audio has gone back "
                     "to Fake Speakers.");
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    // A pick that failed is not the user's device now, any more than one
    // that refused to open.
    CHECK(fix.engine().preferredOutputDevice() == "Fake Speakers");

    OscReply reply;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

TEST_CASE("DeadDevice: a cold switch to a device that never delivers audio goes "
          "back to the old device at the old rate", "[DeadDevice]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Interface").failStart = true;
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 48000);

    auto r = fix.engine().switchDevice("Fake Interface", 44100, 0, false, "__none__");
    INFO(fix.debugMessagesDump());
    CHECK_FALSE(r.success);
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    CHECK(static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 48000);
    CHECK(fix.engine().engineState() == EngineState::Running);

    OscReply reply;
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

TEST_CASE("DeadDevice: a recovery onto a device that never delivers audio is not "
          "reported restored, and the watchdog's next one is once the device ticks",
          "[DeadDevice][recovery]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.callbackWatchdog           = true;
    cfg.watchdogPollMs             = 50;
    cfg.watchdogStallMs            = 500;
    cfg.watchdogRecoveryCooldownMs = 200;
    cfg.watchdogRateWindowMs       = 0;   // not the subject
    // The watchdog polls only when the case says, on the case's clock: it
    // cannot launch a recovery of its own into the user's reopen below.
    cfg.watchdogClockMs            = [sys] { return sys->nowMs(); };
    // Drain the egress on a host thread. With the engine's own gateway the
    // control pass runs once per audio block, so while nothing ticks the
    // replies wait in the ring until the next swap passes them on; this case
    // is about what the recovery says, as it says it.
    cfg.hostDrivesControl          = true;
    EngineFixture fix(cfg);

    // The speakers stop answering, and the user asks for a reopen: it is
    // heard (the device still ticks when it arrives) and runs onto silence.
    sys->at("Fake Speakers").failStart = true;
    OscReply reply;
    fix.send(osc_test::message("/clockwork/devices/reopen"));
    REQUIRE(fix.waitForReply("/clockwork/devices/reopen.done", reply, 15000));
    {
        INFO(fix.debugMessagesDump());
        CHECK(reply.parsed().argInt(0) == 0);
        // Nowhere to go back to: say so, and what to try.
        CHECK(reply.parsed().argString(4) == "No sound is coming out of Fake Speakers. "
                                             "Try choosing it again, or pick another output.");
    }

    // While nothing ticks the engine hears no commands: the watchdog's
    // recovery is the way out. The speakers come back, and it finds them.
    sys->at("Fake Speakers").failStart = false;
    sys->useVirtualTime();
    REQUIRE(fake_audio::runWatchdogUntil(fix.engine(), *sys, 50, 20000, [&] {
        for (auto& r : fix.allReplies())
            if (r.address == "/clockwork/devices/reopen.done" && r.parsed().argInt(0) == 1)
                return true;
        return false;
    }));
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");

    sys->useWallTime();   // the watchdog's part is done; the ping needs the audio thread
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}

TEST_CASE("DeadDevice: a recovery whose pinned device delivers no audio, while the "
          "system default does, is reported restored on the default",
          "[DeadDevice][recovery]") {
    // Recovery opens the system default on a fresh manager, then retargets the
    // pinned device (the gig: pinned speakers, default the headphone jack).
    // The pin is silent; the default it went back to plays. That is audio
    // restored — on the device that plays — not "device down".
    auto sys = makeSimpleSystem();             // the default is Fake Speakers
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.watchdogRecoveryCooldownMs = 0;
    cfg.hostDrivesControl          = true;     // as above: hear the outcome
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().preferredOutputDevice() == "Fake Interface");

    sys->at("Fake Interface").failStart = true;
    OscReply reply;
    fix.send(osc_test::message("/clockwork/devices/reopen"));
    REQUIRE(fix.waitForReply("/clockwork/devices/reopen.done", reply, 20000));
    INFO(fix.debugMessagesDump());
    const auto done = reply.parsed();
    CHECK(done.argInt(0) == 1);
    CHECK(done.argString(1) == "Fake Speakers");
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");

    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", reply));
}
