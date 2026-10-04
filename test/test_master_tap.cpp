// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_master_tap.cpp — the OUT tap on a device host: clockwork writes the
 * master mix into it from the device callback, from boot, held by nobody.
 * The previous protocol had the guest write slot 0 while a client held it;
 * a client now finds the mix in the tap with no verb sent to anyone.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "clockwork_arena.h"
#include "shm_audio_buffer.hpp"
#include <cmath>
#include <vector>

TEST_CASE("master tap: the OUT tap flows from boot on a device host and carries the guest's tone",
          "[tap][engine]") {
    // The case renders every block itself: the tap is measured against what
    // was rendered, not against how much time a thread was given.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    EngineFixture fx(cfg);
    ClockworkRegion taps {};
    REQUIRE(clockwork_client_region(fx.engine().egressClient(), CLOCKWORK_REGION_AUDIO_TAPS, &taps) == CLOCKWORK_OK);
    auto* out = static_cast<shm_audio_buffer*>(taps.base) + CLOCKWORK_TAP_OUT;
    CHECK(out->enabled.load() == 1);
    CHECK(out->channels >= 1);

    // Flowing: every frame rendered is in the tap.
    const uint32_t block  = static_cast<uint32_t>(fx.engine().audioCallback().bufferLength());
    const uint32_t blocks = 48000 * 3 / 10 / block;   // 0.3 s
    const uint64_t a = out->write_position.load();
    fx.pumpBlock(blocks);
    const uint64_t b = out->write_position.load();
    CHECK(b - a == uint64_t(blocks) * block);

    // What plays is what the tap carries.
    shm_audio_buffer_reader reader(out);
    reader.seek_to_live();
    fx.send(osc_test::message("/dummy/tone"));
    fx.pumpBlock(48000 / 5 / block);                  // 0.2 s
    std::vector<float> buf(4096 * SHM_AUDIO_CHANNELS);
    float peak = 0.f;
    for (int i = 0; i < 8; ++i) {
        const uint32_t n = reader.pull(buf.data(), 4096, nullptr);
        for (uint32_t k = 0; k < n * out->channels; ++k) peak = std::max(peak, std::fabs(buf[k]));
        if (!n) break;
    }
    CHECK(peak > 0.05f);
}
