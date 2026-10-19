// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_refusals.cpp — docs/RESTARTS.md through a whole engine.
 *
 * test_restarts.cpp pins the promises against the mechanism (destroy_dsp +
 * rebuild_dsp, on the lanes). These cases make the same promises through
 * ClockworkEngine's cold swap, where the order of the teardown, the drain
 * and the rebuild is the engine's, and where a message arrives through the
 * engine's own door (ingest / sendOSC) rather than the lanes ABI: what a
 * transport's peer, a GUI in the process and the NIF all go through.
 *
 * The swap is a device change on a headless, pumped engine: the rate differs
 * (48000 -> 44100), so the swap is cold, and nothing renders between the
 * send and the swap unless a case pumps.
 */
#include "EngineFixture.h"
#include "clockwork_arena.h"
#include "clockwork_client.h"
#include "clockwork_prefix.h"
#include "clockwork_sys.h"
#include "dsp_api.h"
#include "lanes/lanes.h"
#include "shared_memory.h"   // RingCursors: where the instance's ring stands

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace {

// A refusal reply, if there is one for this address among the replies.
bool refusalFor(EngineFixture& fix, const std::string& addr, std::string* reason) {
    for (const auto& r : fix.allReplies()) {
        if (r.address != CLOCKWORK_SYS("error")) continue;
        const auto p = r.parsed();
        if (p.argCount() < 2 || p.argString(0) != addr) continue;
        if (reason) *reason = p.argString(1);
        return true;
    }
    return false;
}

bool replied(EngineFixture& fix, const std::string& addr) {
    for (const auto& r : fix.allReplies())
        if (r.address == addr) return true;
    return false;
}

// Everything on the reply rings, as a host that reads them itself takes it
// (Config::hostDrainsEgress, clockwork_client_poll): the addresses, in the
// order given.
std::vector<std::string> pollAll(ClockworkClient* rings) {
    std::vector<std::string> out;
    std::array<ClockworkClientMessage, 64> batch{};
    for (;;) {
        const uint32_t n = clockwork_client_poll(rings, batch.data(), static_cast<uint32_t>(batch.size()));
        for (uint32_t i = 0; i < n; ++i)
            out.push_back(osc_test::parseAddress(batch[i].bytes, batch[i].length));
        if (n < batch.size()) return out;
    }
}

long indexOf(const std::vector<std::string>& heard, const std::string& addr) {
    for (size_t i = 0; i < heard.size(); ++i)
        if (heard[i] == addr) return static_cast<long>(i);
    return -1;
}

// An engine whose reply rings a host reads itself, as SuperSonic's server
// does: a reader the engine can neither park nor drain for.
ClockworkEngine::Config hostReadsTheRings() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump  = true;
    cfg.hostDrainsEgress = true;
    return cfg;
}

} // namespace

TEST_CASE("restarts, engine: a message waiting when a cold swap begins is refused to its sender, not delivered to the new instance",
          "[restarts][engine][ColdSwap]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);

    // In the ring, not yet drained: what a client had sent in the moment the
    // instance went.
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));

    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    // Refused to the sender, naming the message and why, before the swap
    // said anything about its new world.
    std::string reason;
    REQUIRE(fix.pollUntil([&] { return refusalFor(fix, "/dummy/ping", &reason); }, 2000));
    CHECK(reason == CLOCKWORK_REFUSED_REBUILDING);

    // And the new instance never saw it.
    fix.pumpBlock(8);
    CHECK_FALSE(replied(fix, "/dummy/pong"));

    // The new instance takes a fresh one.
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    OscReply pong;
    CHECK(fix.waitForReply("/dummy/pong", pong));
}

TEST_CASE("restarts, engine: a message sent while there is no instance is refused at the door, and clockwork's own is answered after the rebuild",
          "[restarts][engine][ColdSwap]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);

    // The window between the teardown and the rebuild: the hook fires there.
    ClockworkStatus guestStatus = CLOCKWORK_OK, sysStatus = CLOCKWORK_E_ARG;
    fix.engine().testRebuildFailure = [&]() -> std::string {
        const auto ping = osc_test::message("/dummy/ping");
        guestStatus = fix.engine().ingest(ping.ptr(), ping.size(), 0);
        const auto sys = osc_test::message(CLOCKWORK_SYS("ping"));
        sysStatus = fix.engine().ingest(sys.ptr(), sys.size(), 0);
        return "";   // no failure: the rebuild goes ahead
    };

    fix.clearReplies();
    const auto swap = switchWhenFree(fix, "", 44100);
    fix.engine().testRebuildFailure = nullptr;
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    // The door said so, as the status, and said so to the asker as the
    // refusal every door uses.
    CHECK(guestStatus == CLOCKWORK_E_NO_INSTANCE);
    std::string reason;
    REQUIRE(fix.pollUntil([&] { return refusalFor(fix, "/dummy/ping", &reason); }, 2000));
    CHECK(reason == CLOCKWORK_REFUSED_NO_INSTANCE);

    // Clockwork's own went through, waited, and is answered by the engine
    // once it renders again.
    CHECK(sysStatus == CLOCKWORK_OK);
    OscReply pong;
    CHECK(fix.waitForReply(CLOCKWORK_SYS("pong"), pong));
    CHECK_FALSE(replied(fix, "/dummy/pong"));
}

TEST_CASE("restarts, engine: restarting names the generation closing, setup the one opened, and the arena is where both are read",
          "[restarts][engine][ColdSwap][generation]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);
    subscribe(fix);   // statechange and setup are broadcasts: heard as a GUI hears them

    // The engine has no counter of its own: what it says is what the arena
    // holds, and a reader of the arena in another process reads the same.
    const auto* h = clockwork_arena_header();
    REQUIRE(h != nullptr);
    REQUIRE(clockwork_arena_has_instance(h));
    const uint32_t gen = clockwork_arena_generation(h);
    CHECK(fix.engine().generation() == gen);

    // Something in the ring at the teardown, to hear the refusal's stamp.
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    // waitForReply hands the matched reply over and drops it from the held
    // list; the others stay held for the walk below.
    OscReply setup;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("setup"), setup, 5000));
    REQUIRE(setup.parsed().argCount() == 3);
    const int32_t opened = setup.parsed().argInt(2);

    // Restarting with the generation that was closing, the refusal stamped
    // with it, setup with the one that opened.
    int32_t restarting = -1, refused = -1;
    for (const auto& r : fix.allReplies()) {
        const auto p = r.parsed();
        if (r.address == CLOCKWORK_SYS("statechange") && p.argString(0) == "restarting") {
            REQUIRE(p.argCount() == 3);
            restarting = p.argInt(2);
        } else if (r.address == CLOCKWORK_SYS("error") && p.argString(0) == "/dummy/ping") {
            REQUIRE(p.argCount() == 3);
            refused = p.argInt(2);
        }
    }
    CHECK(restarting == static_cast<int32_t>(gen));
    CHECK(refused == static_cast<int32_t>(gen));
    CHECK(opened == static_cast<int32_t>(gen + 1));

    // And the arena agrees, as does the engine's own word for it.
    CHECK(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == gen + 1);
    CHECK(fix.engine().generation() == gen + 1);

    // A hot restart would not move it; a statechange that is not a rebuild
    // still names the generation the instance has. The running statechange
    // after this swap names the one that opened.
    bool runningNamed = false;
    for (const auto& r : fix.allReplies()) {
        const auto p = r.parsed();
        if (r.address == CLOCKWORK_SYS("statechange") && p.argString(0) == "running"
            && p.argCount() == 3 && p.argInt(2) == static_cast<int32_t>(gen + 1))
            runningNamed = true;
    }
    CHECK(runningNamed);
}

TEST_CASE("restarts, engine: a client hears setup before anything the new instance says",
          "[restarts][engine][ColdSwap][generation]") {
    // On setup a client drops everything it held of the old instance's
    // (docs/RESTARTS.md, "What a client does"). A reply from the new instance
    // heard before setup is dropped with it: an asset the new instance took
    // goes back to the client's pool while the new instance still reads it.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump   = true;
    cfg.hostDrivesControl = true;
    EngineFixture fix(cfg);
    subscribe(fix);
    // The case is the host from here: a reply pass runs when it says.
    fix.stopHostControl();

    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    // The client has not heard yet, and sends: the new instance takes it.
    fix.send(osc_test::message("/dummy/ping"));
    for (int i = 0; i < 8 && !replied(fix, "/dummy/pong"); ++i) {
        fix.pumpBlock();
        fix.engine().controlPass();
    }

    const auto replies = fix.allReplies();
    INFO(fix.repliesDump());
    const auto at = [&](const std::string& addr) {
        for (size_t i = 0; i < replies.size(); ++i)
            if (replies[i].address == addr) return static_cast<long>(i);
        return -1L;
    };
    const long setup = at(CLOCKWORK_SYS("setup")), pong = at("/dummy/pong");
    REQUIRE(setup >= 0);
    REQUIRE(pong >= 0);
    CHECK(setup < pong);
}

TEST_CASE("restarts, engine: what the old instance said that a host had not read reaches it, before setup",
          "[restarts][engine][ColdSwap]") {
    EngineFixture fix(hostReadsTheRings());
    ClockworkClient* rings = fix.engine().egressClient();
    REQUIRE(rings != nullptr);
    pollAll(rings);   // the boot's

    // The old instance answers, and the host has not read it when the rebuild comes.
    fix.send(osc_test::message("/dummy/ping"));
    fix.pumpBlock();
    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    const auto heard = pollAll(rings);
    INFO([&] { std::string s; for (const auto& a : heard) s += "\n  | " + a; return s; }());
    const long pong = indexOf(heard, "/dummy/pong"), setup = indexOf(heard, CLOCKWORK_SYS("setup"));
    REQUIRE(pong >= 0);
    REQUIRE(setup >= 0);
    CHECK(pong < setup);
}

TEST_CASE("restarts, engine: the old instance's asset releases reach a host that had not read them, before setup",
          "[restarts][engine][ColdSwap]") {
    EngineFixture fix(hostReadsTheRings());
    ClockworkClient* rings = fix.engine().egressClient();
    REQUIRE(rings != nullptr);

    // An asset the old instance holds: its bytes in the lane, committed, and the commit answered.
    std::memset(const_cast<uint8_t*>(fix.engine().guestAssets()), 0x5A, 64);
    osc_test::Builder b;
    b.begin("/clockwork/asset/commit") << int32_t{5} << int32_t{CLOCKWORK_ASSET_RAW} << int32_t{0}
                                       << int32_t{64} << int32_t{0} << int32_t{0} << 0.f;
    fix.send(b.end());
    fix.pumpBlock();
    REQUIRE(indexOf(pollAll(rings), CLOCKWORK_SYS("asset/committed")) >= 0);

    // The rebuild releases it; the host reads nothing until the rebuild is done.
    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    const auto heard = pollAll(rings);
    INFO([&] { std::string s; for (const auto& a : heard) s += "\n  | " + a; return s; }());
    const long released = indexOf(heard, CLOCKWORK_SYS("asset/released")), setup = indexOf(heard, CLOCKWORK_SYS("setup"));
    REQUIRE(released >= 0);
    REQUIRE(setup >= 0);
    CHECK(released < setup);
}

TEST_CASE("restarts, engine: a new instance whose ring has no room for its setup says nothing until it has, then setup first",
          "[restarts][engine][ColdSwap]") {
    EngineFixture fix(hostReadsTheRings());
    fix.expectEgressRefusals();   // the instance's ring is filled on purpose
    ClockworkClient* rings = fix.engine().egressClient();
    REQUIRE(rings != nullptr);
    pollAll(rings);

    // The host stops reading, and the old instance answers until its ring takes nothing more.
    const auto* cursors = reinterpret_cast<const RingCursors*>(
        static_cast<uint8_t*>(clockwork_lanes_base()) + RING_CURSORS_START);
    const auto ping = osc_test::message("/dummy/ping");
    bool full = false;
    for (int i = 0; i < 100000 && !full; ++i) {
        const int32_t before = cursors->out_head.load(std::memory_order_acquire);
        fix.send(ping);
        fix.pumpBlock();
        full = cursors->out_head.load(std::memory_order_acquire) == before;
    }
    REQUIRE(full);

    // The rebuild finds no room for the new instance's setup. Asked something, the new
    // instance renders its blocks and answers nothing while setup waits.
    const auto swap = switchWhenFree(fix, "", 44100);
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);
    fix.send(osc_test::message("/dummy/asset/last"));
    fix.pumpBlock(8);
    auto heard = pollAll(rings);   // the host reads again: the old instance's answers, and room
    CHECK(indexOf(heard, CLOCKWORK_SYS("setup")) < 0);
    CHECK(indexOf(heard, "/dummy/asset/last.reply") < 0);

    // With room, the next block says setup, and then the new instance answers.
    fix.pumpBlock(4);
    heard = pollAll(rings);
    INFO([&] { std::string s; for (const auto& a : heard) s += "\n  | " + a; return s; }());
    const long setup = indexOf(heard, CLOCKWORK_SYS("setup")), answer = indexOf(heard, "/dummy/asset/last.reply");
    REQUIRE(setup >= 0);
    REQUIRE(answer >= 0);
    CHECK(setup < answer);
}
