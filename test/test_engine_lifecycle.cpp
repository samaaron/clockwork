// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_lifecycle.cpp — ClockworkEngine lifecycle.
 *
 * init/shutdown safety, callback wiring, and the round trips that prove the
 * engine is alive after boot: a /dummy/ping answered by the DSP on the audio
 * thread, and a clock query whose reply echoes the id it was sent with.
 */
#include "EngineFixture.h"
#include "clockwork_prefix.h"
#include "lanes/lanes.h"   // post-shutdown ABI guard tests
#include <thread>
#include <chrono>

// =============================================================================
// RAW ENGINE STATE (no fixture — before/after init/shutdown)
// =============================================================================

TEST_CASE("Engine starts in non-running state before init", "[lifecycle]") {
    ClockworkEngine engine;
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("isRunning returns true after init", "[lifecycle]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);
    CHECK(engine.isRunning());
    engine.shutdown();
}

TEST_CASE("isRunning returns false after shutdown", "[lifecycle]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);
    REQUIRE(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Double init is safe", "[lifecycle]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    engine.init(cfg);
    REQUIRE(engine.isRunning());

    // Second call is a no-op (mRunning is already true)
    engine.init(cfg);
    CHECK(engine.isRunning());

    engine.shutdown();
}

TEST_CASE("Double shutdown is safe", "[lifecycle]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;

    engine.init(cfg);
    REQUIRE(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());

    // Second shutdown must not crash
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// shutdown() must not leave the lanes entry points armed against a dead
// engine: memory_initialized gates clockwork_ingress_write/clockwork_tick, and
// with the shm-backed arena the segment is unmapped at the end of shutdown — a
// write that passes the guard afterwards touches unmapped memory.
TEST_CASE("Lanes entry points reject after shutdown (process-local arena)",
          "[lifecycle][lanes]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;   // process-local arena

    engine.init(cfg);
    REQUIRE(engine.isRunning());
    engine.shutdown();

    auto pkt = osc_test::message("/dummy/ping");
    CHECK_FALSE(clockwork_ingress_write(pkt.ptr(), pkt.size(), 0));
}

TEST_CASE("Lanes entry points reject after shutdown (shm-backed arena)",
          "[lifecycle][lanes]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 57123;   // non-zero → arena lives in the public shm segment

    engine.init(cfg);
    REQUIRE(engine.isRunning());
    engine.shutdown();   // unmaps the segment

    // A write that passes the guards here dereferences the unmapped segment
    // (SIGSEGV/SIGBUS), so "returns false" also proves "does not touch the
    // dead arena".
    auto pkt = osc_test::message("/dummy/ping");
    CHECK_FALSE(clockwork_ingress_write(pkt.ptr(), pkt.size(), 0));
}

TEST_CASE("Null onReply callback does not crash", "[lifecycle]") {
    ClockworkEngine engine;
    engine.onReply = nullptr;

    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);

    // A command that produces a reply, with nobody to give it to.
    auto pkt = osc_test::message("/dummy/ping");
    engine.sendOSC(pkt.ptr(), pkt.size());
    // Can't wait for the reply (onReply is null); shutdown drains, and the
    // headless driver processes the message within a few ms.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.shutdown();
    SUCCEED();
}

TEST_CASE("Null onDebug callback does not crash", "[lifecycle]") {
    ClockworkEngine engine;
    engine.onDebug = nullptr;

    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);

    auto pkt = osc_test::message("/dummy/ping");
    engine.sendOSC(pkt.ptr(), pkt.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    engine.shutdown();
    SUCCEED();
}

// =============================================================================
// FIXTURE-BASED (engine booted with callbacks wired)
// =============================================================================

TEST_CASE("Engine answers a ping after boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));
}

TEST_CASE("sendOSC works after init", "[lifecycle]") {
    EngineFixture fx;

    auto pkt = osc_test::message("/dummy/ping");
    fx.engine().sendOSC(pkt.ptr(), pkt.size());

    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));
}

TEST_CASE("onReply callback is wired correctly", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));

    CHECK(r.address == "/dummy/pong");
    CHECK(!r.raw.empty());
}

TEST_CASE("Engine processes multiple sequential pings", "[lifecycle]") {
    EngineFixture fx;

    for (int i = 0; i < 5; ++i) {
        fx.clearReplies();
        fx.send(osc_test::message("/dummy/ping"));
        OscReply r;
        REQUIRE(fx.waitForReply("/dummy/pong", r));
    }
}

TEST_CASE("A clock query echoes its id after a fresh boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 12345));
    OscReply r;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r));
    CHECK(lastInt(r) == 12345);
}

TEST_CASE("/clockwork/notify is acknowledged after boot", "[lifecycle]") {
    EngineFixture fx;

    fx.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply r;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("notify.reply"), r));
}

TEST_CASE("Engine refuses a manual audio block while a driver is rendering", "[lifecycle][pump]") {
    // The default fixture runs the headless driver on its own thread. A block
    // pumped from here would be a second renderer — the engine must refuse it
    // and say so, rather than race the driver across the whole engine state.
    EngineFixture fix;
    fix.engine().pumpAudioBlock();
    const bool refused = fix.pollUntil([&] {
        for (auto& m : fix.debugMessages())
            if (m.find("pumpAudioBlock refused") != std::string::npos) return true;
        return false;
    });
    INFO("engine log:" << fix.debugMessagesDump());
    REQUIRE(refused);
}
