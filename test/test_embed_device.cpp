// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_embed_device.cpp — clockwork_embed_boot: an engine in your process
 * that opens the device and ticks itself, and the client it hands back.
 */
#include <catch2/catch_test_macros.hpp>
#include "OscTestUtils.h"
#include "clockwork_embed.h"
#include <cctype>
#include <chrono>
#include <string>
#include <thread>

TEST_CASE("embed: boot opens the device, ticks itself, answers through its client, and closes", "[embed][engine]") {
    ClockworkEmbedConfig cfg {};
    cfg.struct_bytes = sizeof cfg;
    cfg.sample_rate  = 48000;
    cfg.app_name     = "clockwork-embed-test";
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkEmbed* h = clockwork_embed_boot(&cfg, &st);
    REQUIRE(h != nullptr);
    CHECK(st == CLOCKWORK_OK);
    ClockworkClient* c = clockwork_embed_client(h);
    REQUIRE(c != nullptr);
    CHECK(clockwork_embed_block_size(h) > 0);

    // A machine with no audio driver — a CI runner, typically — boots the
    // engine but never receives a callback, so nothing that depends on the
    // device ticking can hold. Ask what actually opened rather than inferring
    // it from a timeout: an empty device name with no callback frames is the
    // honest answer for "none". Same stance clockwork-client's boot test takes
    // for a build with no device layer (rust/clockwork-client/tests/boot.rs).
    ClockworkEmbedDevice opened {};
    opened.struct_bytes = sizeof opened;
    const bool haveDevice = clockwork_embed_device(h, &opened) == CLOCKWORK_OK
                            && opened.device[0] != '\0' && opened.buffer_frames > 0;
    if (!haveDevice)
        WARN("no audio device opened; the ticking and answering checks are skipped");

    // It ticks: the process count climbs with no render from us. Polled
    // rather than slept on a fixed window, because the first blocks of a
    // device start are warmup callbacks that emit silence without ticking
    // (JuceAudioCallback, mCallbackCount < 4), so how long the count takes
    // to move is the hardware buffer's period times four — 43 ms on a
    // 512-frame device, 213 ms on the 2560-frame buffer Windows' DirectSound
    // default hands out, which a fixed 200 ms sleep loses to.
    if (haveDevice) {
        uint32_t m1[4] = {}, m2[4] = {};
        REQUIRE(clockwork_client_metrics(c, m1, 4) >= 1);
        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(clockwork_client_metrics(c, m2, 4) >= 1);
            if (m2[0] > m1[0]) break;
        }
        CHECK(m2[0] > m1[0]);
    }
    CHECK(clockwork_embed_render(h, nullptr, 0, nullptr, 0, 64) == 0);   // a device host renders itself

    // And it answers.
    if (haveDevice) {
        const auto ping = osc_test::message("/dummy/ping");
        REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 0x5a5a) == CLOCKWORK_OK);
        bool pong = false;
        for (int i = 0; i < 50 && !pong; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ClockworkClientMessage m[8];
            const uint32_t n = clockwork_client_poll(c, m, 8);
            for (uint32_t k = 0; k < n; ++k)
                if (osc_test::parseAddress(m[k].bytes, m[k].length) == "/dummy/pong") pong = true;
        }
        CHECK(pong);
    }

    // A second boot while this one runs is refused; after close it is not.
    CHECK(clockwork_embed_boot(&cfg, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_PERM);
    clockwork_embed_close(h);
    h = clockwork_embed_boot(&cfg, &st);
    REQUIRE(h != nullptr);
    clockwork_embed_close(h);
}

// The device an engine booted on, read back through the door.
static ClockworkEmbedDevice deviceOf(ClockworkEmbed* h) {
    ClockworkEmbedDevice d {};
    d.struct_bytes = sizeof d;
    REQUIRE(clockwork_embed_device(h, &d) == CLOCKWORK_OK);
    return d;
}

static ClockworkEmbed* bootOn(const char* driver, ClockworkStatus* st) {
    ClockworkEmbedConfig cfg {};
    cfg.struct_bytes = sizeof cfg;
    cfg.sample_rate  = 48000;
    cfg.app_name     = "clockwork-embed-test";
    cfg.driver       = driver;
    return clockwork_embed_boot(&cfg, st);
}

TEST_CASE("embed: boot reports the device it opened, and opens the driver it is asked for", "[embed][engine]") {
    ClockworkStatus st = CLOCKWORK_OK;

    // The platform default, whatever it is here, and a full account of it.
    ClockworkEmbed* h = bootOn(nullptr, &st);
    REQUIRE(h != nullptr);
    const ClockworkEmbedDevice dflt = deviceOf(h);

    // A machine with no audio device at all — a CI runner, typically — boots
    // and reports that nothing opened. Every claim below is about WHICH device
    // was chosen and whether a name resolves to it, so on such a host there is
    // no question to answer: say so and stop, rather than assert against a
    // device that is not there. The same stance rust/clockwork-client's boot
    // test takes for a build with no device layer.
    if (dflt.device[0] == 0 || dflt.buffer_frames == 0) {
        WARN("no audio device opened here; the driver read-back is not tested");
        clockwork_embed_close(h);
        return;
    }

    CHECK(dflt.driver[0] != 0);
    CHECK(dflt.device[0] != 0);
    CHECK(dflt.sample_rate > 0.0);
    CHECK(dflt.buffer_frames > 0);
    CHECK(dflt.block_frames == clockwork_embed_block_size(h));
    CHECK(dflt.output_channels >= 1);
    // A struct that says it is smaller than this build's is refused, zeroed.
    ClockworkEmbedDevice small {};
    small.struct_bytes = 4;
    CHECK(clockwork_embed_device(h, &small) == CLOCKWORK_E_ARG);
    CHECK(small.driver[0] == 0);
    clockwork_embed_close(h);
    const std::string defaultDriver = dflt.driver;

    // Asked for by its own name: the same driver.
    h = bootOn(defaultDriver.c_str(), &st);
    REQUIRE(h != nullptr);
    CHECK(std::string(deviceOf(h).driver) == defaultDriver);
    clockwork_embed_close(h);

    // Resolution is case-insensitive when that is unambiguous.
    std::string lower = defaultDriver;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    h = bootOn(lower.c_str(), &st);
    REQUIRE(h != nullptr);
    CHECK(std::string(deviceOf(h).driver) == defaultDriver);
    clockwork_embed_close(h);

    // A name that is nothing keeps the default rather than refusing to boot.
    h = bootOn("no-such-driver", &st);
    REQUIRE(h != nullptr);
    CHECK(st == CLOCKWORK_OK);
    CHECK(std::string(deviceOf(h).driver) == defaultDriver);
    clockwork_embed_close(h);

#if defined(_WIN32)
    // Windows always has DirectSound alongside WASAPI, so the field can be
    // seen to CHOOSE, not merely to repeat the default.
    h = bootOn("DirectSound", &st);
    REQUIRE(h != nullptr);
    CHECK(std::string(deviceOf(h).driver) == "DirectSound");
    clockwork_embed_close(h);
#endif
}
