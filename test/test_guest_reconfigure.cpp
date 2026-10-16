// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_guest_reconfigure.cpp — a device change the guest instance survives.
 *
 * docs/RESTARTS.md: a device change clockwork would otherwise answer with a
 * cold restart is a HOT one when the instance declared it survives that
 * kind of change (DspInfo::survives, dsp_api.h). The device is reopened,
 * the instance is told the new facts (dsp_reconfigure) with audio stopped,
 * and it keeps everything it holds; clockwork's own regions that follow the
 * device are laid out again for it; no client is told to rebuild. If the
 * instance refuses, the swap is cold from there: what it would have been.
 *
 * The dummy guest survives a channel-count change and refuses above eight
 * outputs; it does not survive a rate change, which every cold-swap case in
 * this suite drives one with. The fake devices supply the channel counts.
 */
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "clockwork_prefix.h"
#include "lanes/lanes.h"
#include "shared_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

// What the instance says about itself: how many times it was told new
// device facts, and the output ceiling it renders to now.
struct Told { int32_t times = -1, outputs = -1; };
Told askTold(EngineFixture& fix) {
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/reconfigured"));
    OscReply r;
    REQUIRE(fix.waitForReply("/dummy/reconfigured.reply", r, 5000));
    const auto p = r.parsed();
    REQUIRE(p.argCount() == 2);
    return { p.argInt(0), p.argInt(1) };
}

bool heard(EngineFixture& fix, const char* address) {
    for (const auto& r : fix.allReplies())
        if (r.address == address) return true;
    return false;
}

} // namespace

TEST_CASE("guest reconfigure: a channel-count change the instance survives is a hot restart that tells it",
          "[restarts][survives][hot]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Interface").maxOutputChannels = 4;   // the speakers have 2
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);
    REQUIRE(clockwork_guest_survives() == DSP_SURVIVES_CHANNEL_CHANGE);
    const uint32_t gen = fix.engine().generation();
    REQUIRE(askTold(fix).times == 0);

    fix.clearReplies();
    const auto swap = switchWhenFree(fix, "Fake Interface", 48000);
    INFO(fix.debugMessagesDump());
    REQUIRE(swap.success);
    CHECK(swap.type == SwapType::Hot);

    // The same instance, told once, rendering to four outputs; the
    // generation it opened is the one still open; nobody was told to
    // rebuild, and nothing was refused.
    const auto told = askTold(fix);
    CHECK(told.times == 1);
    CHECK(told.outputs == 4);
    CHECK(fix.engine().generation() == gen);
    CHECK_FALSE(heard(fix, CLOCKWORK_SYS("setup")));
    CHECK_FALSE(heard(fix, CLOCKWORK_SYS("error")));
    // Clockwork's own picture of the device followed it: the channel map's
    // device words, which a client and the guest both read.
    CHECK(clockwork_channel_map()->device_out.load() == 4u);
}

TEST_CASE("guest reconfigure: a change the instance refuses becomes the cold restart it would have been",
          "[restarts][survives][ColdSwap]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Interface").maxOutputChannels = 16;  // above what the dummy takes live
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);
    const uint32_t gen = fix.engine().generation();

    fix.clearReplies();
    const auto swap = switchWhenFree(fix, "Fake Interface", 48000);
    INFO(fix.debugMessagesDump());
    REQUIRE(swap.success);
    CHECK(swap.type == SwapType::Cold);

    // A new instance, built at sixteen, that was never told anything; the
    // generation moved on; the clients heard restarting and setup, as for
    // any cold restart.
    OscReply setup;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("setup"), setup, 5000));
    const auto told = askTold(fix);
    CHECK(told.times == 0);
    CHECK(told.outputs == 16);
    CHECK(fix.engine().generation() == gen + 1);
    CHECK(clockwork_channel_map()->device_out.load() == 16u);
}
