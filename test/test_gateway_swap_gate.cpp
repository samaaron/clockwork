// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_gateway_swap_gate.cpp — the control pass never waits on a device swap.
 *
 * A cold swap holds the swap gate and then parks the control pass (the
 * gateway) before it rebuilds the shared-memory arena the pass drains. So a
 * pass that waits for the swap gate waits for the swap, and the swap waits
 * for it: the park gives up after 2 s and rebuilds the arena under a pass
 * still in flight, and a run of swaps stalls the engine's replies and logging
 * for as long as they last. Found by the device-event race case
 * (test_device_events.cpp) on CI, 2026-10-04: the Tracks bridge read the
 * device's buffer size through currentDevice() every 64 passes. Asking the
 * engine about its devices over OSC did the same.
 *
 * Each case holds the gate as a swap does, and needs the pass to keep
 * running: every pong it waits for is delivered by a pass.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <string>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

// One round trip through the control pass: the pong is drained by a pass.
bool pong(EngineFixture& fix) {
    fix.send(osc_test::message("/dummy/ping"));
    OscReply reply;
    return fix.waitForReply("/dummy/pong", reply, 2000);
}

}  // namespace

TEST_CASE("Gateway: passes keep running while a swap holds the gate",
          "[Gateway][SwapGate]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(pong(fix));

    auto swap = fix.engine().testHoldSwapGate();
    // A hundred passes, at least: everything a pass does on a timer of its
    // own (the Tracks bridge looks at the device every 64) comes round.
    for (int i = 0; i < 100; ++i) {
        INFO("round trip " << i);
        REQUIRE(pong(fix));
    }
}

TEST_CASE("Gateway: asking about the devices during a swap is answered after "
          "it, and the pass runs on meanwhile", "[Gateway][SwapGate]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(pong(fix));
    fix.clearReplies();

    auto swap = fix.engine().testHoldSwapGate();
    for (const char* verb : { CLOCKWORK_SYS("devices/current"), CLOCKWORK_SYS("devices/list"),
                              CLOCKWORK_SYS("drivers/list") })
        fix.send(osc_test::message(verb));
    REQUIRE(pong(fix));
    REQUIRE(pong(fix));

    // The answers describe the device the swap leaves behind.
    swap.unlock();
    OscReply current, listed, drivers;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/current.reply"), current, 5000));
    CHECK(current.parsed().argString(0) == "Fake Speakers");
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/list.done"), listed, 5000));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("drivers/list.reply"), drivers, 5000));
    CHECK(drivers.parsed().argString(0) == "FakeDriver");
}
