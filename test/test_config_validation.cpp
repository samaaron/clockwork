// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_config_validation.cpp — ClockworkEngine::Config edge cases
 *
 * Tests that the engine boots and shuts down cleanly with various Config
 * parameter combinations, and verifies default values are correct.
 */
#include "EngineFixture.h"
#include "GuestConfigText.h"
#include "shared_memory.h"   // GUEST_CONFIG_SIZE

#include <stdexcept>
#include <thread>
#include <chrono>

// ── 1. Default config values ────────────────────────────────────────────────

TEST_CASE("Default Config values are correct", "[config]") {
    ClockworkEngine::Config cfg;

    CHECK(cfg.sampleRate             == 48000);
    CHECK(cfg.bufferSize             == 0);   // 0 = auto (smallest multiple of 128)
    CHECK(cfg.udpPort                == 57110);
    // Default is "auto" (-1) — JUCE/CoreAudio clamps to the device's real
    // channel count. Headless test fixtures override these to get
    // predictable 2-in / 2-out behaviour.
    CHECK(cfg.numOutputChannels      == ClockworkEngine::kAutoChannelCount);
    CHECK(cfg.numInputChannels       == ClockworkEngine::kAutoChannelCount);
    CHECK(cfg.headless               == false);
    CHECK(cfg.guestConfig.empty());   // the guest's defaults
}

// ── 2. Minimum viable config ────────────────────────────────────────────────

TEST_CASE("Engine boots with minimum viable config", "[config]") {
    ClockworkEngine engine;
    bool gotReply = false;
    engine.onReply = [&](const uint8_t*, uint32_t) { gotReply = true; };

    ClockworkEngine::Config cfg;
    cfg.headless    = true;
    cfg.udpPort     = 0;
    clockwork::guest_config_text::set(cfg.guestConfig, "maxNodes", "4");
    clockwork::guest_config_text::set(cfg.guestConfig, "numBuffers", "4");
    clockwork::guest_config_text::set(cfg.guestConfig, "maxGraphDefs", "4");
    clockwork::guest_config_text::set(cfg.guestConfig, "maxWireBufs", "4");
    clockwork::guest_config_text::set(cfg.guestConfig, "numRGens", "4");
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 2b. A guest config that does not fit its region ────────────────────────
// Config::guestConfig is carried to the guest in a region of GUEST_CONFIG_SIZE
// bytes. One that does not fit, NUL included, is refused at init, as the
// web side refuses it (writeGuestConfigToMemory): truncated instead, it
// would boot a guest on a config cut mid-line — realTimeMemorySize=65536
// read as realTimeMemorySize=6 — which is the silent misconfiguration the
// text block exists to rule out.

TEST_CASE("A guest config larger than its region refuses init", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    // valid lines, more of them than the region holds
    while (cfg.guestConfig.size() + 1 <= GUEST_CONFIG_SIZE)
        cfg.guestConfig += "# a comment line the guest ignores, one of many\n";
    clockwork::guest_config_text::set(cfg.guestConfig, "maxNodes", "4");
    REQUIRE(cfg.guestConfig.size() + 1 > GUEST_CONFIG_SIZE);

    CHECK_THROWS_AS(engine.init(cfg), std::runtime_error);
    CHECK_FALSE(engine.isRunning());
    engine.shutdown();
}

// ── 3. Large config values ──────────────────────────────────────────────────

TEST_CASE("Engine boots with large config values", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    clockwork::guest_config_text::set(cfg.guestConfig, "maxNodes", "4096");
    clockwork::guest_config_text::set(cfg.guestConfig, "numBuffers", "4096");
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 4. Sample rate 44100 ────────────────────────────────────────────────────

TEST_CASE("Engine boots at 44100 Hz", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.sampleRate = 44100;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots at 96000 Hz", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.sampleRate = 96000;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 5. Buffer sizes ─────────────────────────────────────────────────────────

TEST_CASE("Engine boots with bufferSize=64", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.bufferSize = 64;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with bufferSize=256", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.bufferSize = 256;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with bufferSize=512", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless   = true;
    cfg.udpPort    = 0;
    cfg.bufferSize = 512;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 6. headless=true (default test mode) ────────────────────────────────────

TEST_CASE("headless=true skips audio device", "[config]") {
    EngineFixture fx;

    // Engine is running in headless mode — verify it responds to /dummy/ping
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));

}

// ── 7. udpPort=0 disables UDP listener ──────────────────────────────────────

TEST_CASE("udpPort=0 disables UDP listener", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);

    // Engine should still be running, just without UDP
    CHECK(engine.isRunning());

    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 8. Mono output (numOutputChannels=1) ────────────────────────────────────

TEST_CASE("Engine boots with mono output", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless          = true;
    cfg.udpPort           = 0;
    cfg.numOutputChannels = 1;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 9. No input channels ───────────────────────────────────────────────────

TEST_CASE("Engine boots with zero input channels", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless         = true;
    cfg.udpPort          = 0;
    cfg.numInputChannels = 0;
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 11. numControlBusChannels variations ────────────────────────────────────

TEST_CASE("Engine boots with small numControlBusChannels", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless              = true;
    cfg.udpPort               = 0;
    clockwork::guest_config_text::set(cfg.guestConfig, "numControlBusChannels", "128");
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

// ── 12. realTimeMemorySize variations ───────────────────────────────────────

TEST_CASE("Engine boots with small realTimeMemorySize", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless           = true;
    cfg.udpPort            = 0;
    clockwork::guest_config_text::set(cfg.guestConfig, "realTimeMemorySize", "256");
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}

TEST_CASE("Engine boots with large realTimeMemorySize", "[config]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    cfg.headless           = true;
    cfg.udpPort            = 0;
    clockwork::guest_config_text::set(cfg.guestConfig, "realTimeMemorySize", "32768");
    engine.init(cfg);

    CHECK(engine.isRunning());
    engine.shutdown();
    CHECK_FALSE(engine.isRunning());
}
