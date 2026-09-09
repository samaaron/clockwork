// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_audio_taps.cpp — the audio taps are clockwork's, written at the
 * device edge inside every tick, on every host.
 *
 * Through the lanes ABI alone, which is what the worklet, a plugin and an
 * embedder drive: what the guest rendered for the device is in the OUT tap
 * after the tick, and what the host put on the input bus before the tick is
 * in the IN tap after it. No guest verb, no hold, no flag a client sets.
 */
#include <catch2/catch_test_macros.hpp>
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"
#include "shared_memory.h"
#include "shm_audio_buffer.hpp"
#include <cmath>
#include <vector>

namespace {
shm_audio_buffer* tap(uint32_t slot) {
    lanes_test::boot();
    const auto* h = clockwork_arena_header();
    const auto* e = clockwork_arena_find(h, CLOCKWORK_ARENA_AUDIO_TAPS);
    REQUIRE(e != nullptr);
    REQUIRE(slot < e->geom[CLOCKWORK_GEOM_TAPS_SLOTS]);
    auto* base = static_cast<uint8_t*>(clockwork_lanes_base());
    return reinterpret_cast<shm_audio_buffer*>(base + e->offset + slot * e->geom[CLOCKWORK_GEOM_TAPS_SLOT_BYTES]);
}
}

TEST_CASE("audio taps: both are formatted at boot at the device's channel count, and flow from the first tick",
          "[lanes][tap]") {
    auto* out = tap(CLOCKWORK_TAP_OUT);
    auto* in  = tap(CLOCKWORK_TAP_IN);
    CHECK(out->enabled.load() == 1);
    CHECK(in->enabled.load() == 1);
    CHECK(out->channels == lanes_test::kOutChannels);
    CHECK(in->channels == lanes_test::kInChannels);
    CHECK(out->sample_rate == static_cast<uint32_t>(lanes_test::kSampleRate));
    CHECK(out->capacity_frames == SHM_AUDIO_FRAMES);

    const uint64_t before = out->write_position.load();
    lanes_test::tick();
    CHECK(out->write_position.load() == before + clockwork_block_size());
    CHECK(in->write_position.load() >= clockwork_block_size());
}

TEST_CASE("audio taps: the IN tap carries what the host put on the input bus, channel for channel",
          "[lanes][tap]") {
    auto* in = tap(CLOCKWORK_TAP_IN);
    const uint32_t bl = clockwork_block_size();
    float* bus = clockwork_audio_in();
    REQUIRE(bus != nullptr);
    // A ramp on channel 0, its negative on channel 1: distinguishable and
    // channel-ordered.
    for (uint32_t f = 0; f < bl; ++f) {
        bus[f]      =  static_cast<float>(f + 1) / static_cast<float>(bl);
        bus[bl + f] = -static_cast<float>(f + 1) / static_cast<float>(bl);
    }
    shm_audio_buffer_reader r(in);
    r.seek_to_live();
    lanes_test::tick();
    std::vector<float> got(bl * lanes_test::kInChannels, 0.0f);
    uint64_t gap = 0;
    REQUIRE(r.pull(got.data(), bl, &gap) == bl);
    CHECK(gap == 0);
    for (uint32_t f = 0; f < bl; ++f) {
        INFO("frame " << f);
        REQUIRE(got[f * 2]     == static_cast<float>(f + 1) / static_cast<float>(bl));
        REQUIRE(got[f * 2 + 1] == -static_cast<float>(f + 1) / static_cast<float>(bl));
    }
    for (uint32_t i = 0; i < bl * lanes_test::kInChannels; ++i) bus[i] = 0.0f;
}

TEST_CASE("audio taps: the OUT tap carries what the guest rendered for the device, block for block",
          "[lanes][tap]") {
    auto* out = tap(CLOCKWORK_TAP_OUT);
    const uint32_t bl = clockwork_block_size();
    // The dummy guest's pulse: a known non-silent signal (test_audio_path).
    const auto pulse = osc_test::message("/dummy/pulse", int32_t{2}, int32_t{10});
    REQUIRE(lanes_test::ingress(pulse.ptr(), pulse.size(), 1));
    shm_audio_buffer_reader r(out);
    r.seek_to_live();
    float peak = 0.0f;
    std::vector<float> got(bl * lanes_test::kOutChannels, 0.0f);
    for (int b = 0; b < 64; ++b) {
        lanes_test::tick();
        const float* rendered = clockwork_audio_out();
        REQUIRE(r.pull(got.data(), bl, nullptr) == bl);
        // The tap is the rendered block, interleaved: nothing more, nothing less.
        for (uint32_t f = 0; f < bl; ++f)
            for (uint32_t c = 0; c < lanes_test::kOutChannels; ++c) {
                REQUIRE(got[f * lanes_test::kOutChannels + c] == rendered[c * bl + f]);
                peak = std::max(peak, std::fabs(got[f * lanes_test::kOutChannels + c]));
            }
    }
    CHECK(peak > 0.0f);
    // The pulse the rest of the suite assumes (test_audio_path.cpp's
    // defaults): this fixture is booted once for the whole binary.
    const auto restore = osc_test::message("/dummy/pulse", int32_t{10}, int32_t{500});
    REQUIRE(lanes_test::ingress(restore.ptr(), restore.size(), 1));
    lanes_test::tick();
    lanes_test::drainRt();
}
