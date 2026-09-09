// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_embed.cpp — an engine in your process (clockwork_embed.h), the
 * host-ticks way: attach, render buffers of any size, and talk to it
 * through the client the handle hands back.
 *
 * This binary's engine is booted once by the lanes fixture, so attach here
 * joins it; what the tests pin is what a plugin needs from the door: the
 * geometry it actually got, a client that works, output that is exact for
 * any buffer size, input that reaches the guest, and a clean close.
 */
#include <catch2/catch_test_macros.hpp>
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "clockwork_embed.h"
#include "clockwork_arena.h"
#include "shm_audio_buffer.hpp"
#include "lanes/lanes.h"
#include <cmath>
#include <numeric>
#include <cstring>
#include <vector>

namespace {
ClockworkEmbedConfig config() {
    ClockworkEmbedConfig c {};
    c.struct_bytes      = sizeof c;
    c.sample_rate       = lanes_test::kSampleRate;
    // The fixture's own geometry: an attach with nobody attached RE-LAYS
    // the engine out to what it asks for, and the fixture counts frames in
    // blocks of its own size.
    c.block_size        = lanes_test::kBufLength;
    c.input_channels    = lanes_test::kInChannels;
    c.output_channels   = lanes_test::kOutChannels;
    c.max_render_frames = 1024;
    return c;
}
struct Handle {
    ClockworkEmbed* h = nullptr;
    ClockworkStatus st = CLOCKWORK_OK;
    Handle() { lanes_test::boot(); h = clockwork_embed_attach(&cfg, &st); }
    ~Handle() { clockwork_embed_close(h); }
    ClockworkEmbedConfig cfg = config();
};
shm_audio_buffer* outTap() {
    const auto* e = clockwork_arena_find(clockwork_arena_header(), CLOCKWORK_ARENA_AUDIO_TAPS);
    REQUIRE(e != nullptr);
    return reinterpret_cast<shm_audio_buffer*>(static_cast<uint8_t*>(clockwork_lanes_base()) + e->offset) + CLOCKWORK_TAP_OUT;
}
}

TEST_CASE("embed: attach hands back a handle with the engine's geometry and a working client", "[embed]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    CHECK(e.st == CLOCKWORK_OK);
    CHECK(clockwork_embed_sample_rate(e.h) == lanes_test::kSampleRate);
    CHECK(clockwork_embed_block_size(e.h) == clockwork_block_size());
    ClockworkClient* c = clockwork_embed_client(e.h);
    REQUIRE(c != nullptr);

    // A verb through the client, a block through render, the reply through the client.
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 0x5a5a) == CLOCKWORK_OK);
    std::vector<float> l(64), r(64);
    float* out[2] = { l.data(), r.data() };
    REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, 64) == 64);
    // No device behind an attached handle, and the read-back says so.
    ClockworkEmbedDevice dev {};
    dev.struct_bytes = sizeof dev;
    CHECK(clockwork_embed_device(e.h, &dev) == CLOCKWORK_E_ARG);
    lanes_test::ticked(64);
    ClockworkClientMessage m[8];
    uint32_t n = 0;
    for (int i = 0; i < 4 && n == 0; ++i) n = clockwork_client_poll(c, m, 8);
    REQUIRE(n >= 1);
    bool pong = false;
    for (uint32_t i = 0; i < n; ++i)
        if (osc_test::parseAddress(m[i].bytes, m[i].length) == "/dummy/pong") pong = true;
    CHECK(pong);
}

TEST_CASE("embed: a second attach while one holds the engine is refused, and possible after close", "[embed]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkStatus st = CLOCKWORK_OK;
    CHECK(clockwork_embed_attach(&e.cfg, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_PERM);
    clockwork_embed_close(e.h);
    e.h = clockwork_embed_attach(&e.cfg, &st);
    REQUIRE(e.h != nullptr);
    CHECK(st == CLOCKWORK_OK);
}

TEST_CASE("embed: a tap on the handle's client watches both rings without taking", "[embed]") {
    // The same taps a segment client opens, on the client an embedder is
    // handed: the ingress ring is watched between the send and the block
    // that consumes it, the egress ring after; and neither shortens what
    // the handle's own poll then takes.
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkClient* c = clockwork_embed_client(e.h);
    REQUIRE(c != nullptr);
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClientTap* in = clockwork_client_tap_open(c, CLOCKWORK_REGION_INGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);
    ClockworkClientTap* out = clockwork_client_tap_open(c, CLOCKWORK_REGION_EGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);

    const uint32_t origin = 0x5245u;
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), origin) == CLOCKWORK_OK);
    ClockworkClientMessage m[8];
    uint32_t n = clockwork_client_tap_poll(in, m, 8);
    REQUIRE(n == 1);
    CHECK(osc_test::parseAddress(m[0].bytes, m[0].length) == "/dummy/ping");
    CHECK(m[0].origin == origin);

    float l[64], r[64];
    float* o[2] = { l, r };
    REQUIRE(clockwork_embed_render(e.h, o, 2, nullptr, 0, 64) == 64);
    lanes_test::ticked(64);
    auto sawPong = [&](uint32_t count) {
        for (uint32_t i = 0; i < count; ++i)
            if (osc_test::parseAddress(m[i].bytes, m[i].length) == "/dummy/pong" && m[i].origin == origin)
                return true;
        return false;
    };
    n = clockwork_client_tap_poll(out, m, 8);
    CHECK(sawPong(n));
    CHECK(clockwork_client_tap_missed(in) + clockwork_client_tap_missed(out) == 0);
    // Watching took nothing: the consumer still gets the reply.
    n = clockwork_client_poll(c, m, 8);
    CHECK(sawPong(n));

    clockwork_client_tap_close(in);
    clockwork_client_tap_close(out);
}

TEST_CASE("embed: render answers any buffer size with exactly the engine's output stream", "[embed]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    // The dummy's pulse is on by default; whatever it renders, the OUT tap
    // holds the stream block by block, and render must hand back that
    // stream cut at the host's sizes — no frame dropped, doubled or shifted.
    shm_audio_buffer_reader tap(outTap());
    tap.seek_to_live();
    // Odd sizes, then enough to cross a pulse period (500 ms) so the stream
    // is not all silence.
    std::vector<uint32_t> sizes = { 7, 64, 129, 300, 500, 1, 1024, 63 };
    while (std::accumulate(sizes.begin(), sizes.end(), 0u) < 26000u) sizes.push_back(1000);
    std::vector<float> gotL, gotR;
    for (uint32_t n : sizes) {
        std::vector<float> l(n), r(n);
        float* out[2] = { l.data(), r.data() };
        REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, n) == n);
        gotL.insert(gotL.end(), l.begin(), l.end());
        gotR.insert(gotR.end(), r.begin(), r.end());
    }
    // The engine ticked whole blocks: the fixture's count follows.
    lanes_test::ticked((gotL.size() + clockwork_block_size() - 1) / clockwork_block_size() * clockwork_block_size());
    // Everything the taps saw, interleaved: at least as many frames as served.
    std::vector<float> stream(gotL.size() * 2 + clockwork_block_size() * 2);
    uint64_t gap = 0;
    const uint32_t frames = tap.pull(stream.data(), static_cast<uint32_t>(stream.size() / 2), &gap);
    REQUIRE(gap == 0);
    REQUIRE(frames >= gotL.size());
    float peak = 0.0f;
    for (size_t f = 0; f < gotL.size(); ++f) {
        INFO("frame " << f);
        REQUIRE(gotL[f] == stream[f * 2]);
        REQUIRE(gotR[f] == stream[f * 2 + 1]);
        peak = std::max(peak, std::fabs(gotL[f]));
    }
    CHECK(peak > 0.0f);
}

TEST_CASE("embed: host input reaches the guest, a block late at most", "[embed]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    const uint32_t block = clockwork_embed_block_size(e.h);
    // Constant input on both channels; after a few blocks the IN tap's newest
    // block is that constant (the first block may be padded with silence).
    const auto* te = clockwork_arena_find(clockwork_arena_header(), CLOCKWORK_ARENA_AUDIO_TAPS);
    auto* inTap = reinterpret_cast<shm_audio_buffer*>(static_cast<uint8_t*>(clockwork_lanes_base()) + te->offset) + CLOCKWORK_TAP_IN;
    shm_audio_buffer_reader reader(inTap);
    reader.seek_to_live();
    std::vector<float> a(block * 3, 0.25f), b(block * 3, -0.5f);
    const float* in[2] = { a.data(), b.data() };
    std::vector<float> l(block * 3), r(block * 3);
    float* out[2] = { l.data(), r.data() };
    REQUIRE(clockwork_embed_render(e.h, out, 2, in, 2, block * 3) == block * 3);
    lanes_test::ticked(block * 3);
    std::vector<float> got(block * 3 * 2);
    const uint32_t frames = reader.pull(got.data(), block * 3, nullptr);
    REQUIRE(frames == block * 3);
    // The last block is the input, channel for channel.
    for (uint32_t f = block * 2; f < block * 3; ++f) {
        INFO("frame " << f);
        REQUIRE(got[f * 2]     == 0.25f);
        REQUIRE(got[f * 2 + 1] == -0.5f);
    }
    // And the whole of it is either the input or the initial silence, never garbage.
    for (uint32_t f = 0; f < block * 3; ++f)
        REQUIRE((got[f * 2] == 0.25f || got[f * 2] == 0.0f));
    // Leave the bus as the rest of this binary expects it.
    if (float* bus = clockwork_audio_in()) std::memset(bus, 0, sizeof(float) * block * lanes_test::kInChannels);
}
