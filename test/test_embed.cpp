// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_embed.cpp — an engine in your process (clockwork_embed.h), the
 * host-ticks way: attach, render buffers of any size, and talk to it
 * through the client the handle hands back.
 *
 * Each handle makes the engine's core and closes it, so these cases live in
 * a binary where nothing else holds one (clockwork_attach_tests). What they
 * pin is what a plugin needs from the door: the geometry it actually got, a
 * client that works, output that is exact for any buffer size, input that
 * reaches the guest, and a clean close.
 */
#include <catch2/catch_test_macros.hpp>
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "clockwork_embed.h"
#include "clockwork_arena.h"
#include "dsp_api.h"
#include "shm_audio_buffer.hpp"
#include "shared_memory.h"   // clockwork::kDefaultLayout
#include "lanes/lanes.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstring>
#include <string>
#include <vector>

namespace {
ClockworkEmbedConfig config() {
    ClockworkEmbedConfig c {};
    c.struct_bytes      = sizeof c;
    c.sample_rate       = lanes_test::kSampleRate;
    // The geometry the lanes fixture's cases are written against.
    c.block_size        = lanes_test::kBufLength;
    c.input_channels    = lanes_test::kInChannels;
    c.output_channels   = lanes_test::kOutChannels;
    c.max_render_frames = 1024;
    return c;
}
struct Handle {
    ClockworkEmbed* h = nullptr;
    ClockworkStatus st = CLOCKWORK_OK;
    Handle() { h = clockwork_embed_attach(&cfg, &st); }
    ~Handle() { clockwork_embed_close(h); }
    ClockworkEmbedConfig cfg = config();
};
// A slot of the engine's scope, where the handle's client reads it: the
// region is its slots end to end.
shm_audio_buffer* scopeSlot(ClockworkEmbed* h, uint32_t slot) {
    ClockworkRegion r {};
    REQUIRE(clockwork_client_region(clockwork_embed_client(h), CLOCKWORK_REGION_SCOPE, &r) == CLOCKWORK_OK);
    REQUIRE(r.base != nullptr);
    return reinterpret_cast<shm_audio_buffer*>(static_cast<uint8_t*>(r.base) + slot * (r.bytes / SCOPE_SLOTS));
}
// The ring an attached engine's scope slot was laid out for: the target's
// default profile.
shm_audio_buffer_reader scopeReader(ClockworkEmbed* h, uint32_t slot) {
    return shm_audio_buffer_reader(scopeSlot(h, slot), clockwork::kDefaultLayout.profile.scope_frames,
                                   clockwork::kDefaultLayout.profile.scope_channels);
}
// The reply at `address` that answers `origin`, read off a client or a tap
// by `poll`. The egress is one stream for everything that talks to the
// engine, and in this binary for every test before this one too: whatever is
// ahead of the reply is not this test's, so it reads until the reply or the
// end of what is there, and says what it passed if the reply never came.
template <typename Poll>
bool takeReply(Poll poll, uint32_t origin, const std::string& address) {
    ClockworkClientMessage m[64];
    std::string passed;
    for (uint32_t n; (n = poll(m, 64)) > 0;)
        for (uint32_t i = 0; i < n; ++i) {
            const auto a = osc_test::parseAddress(m[i].bytes, m[i].length);
            if (a == address && m[i].origin == origin) return true;
            passed += a + " (" + std::to_string(m[i].origin) + ") ";
        }
    UNSCOPED_INFO("no " << address << " answering " << origin << "; passed: " << passed);
    return false;
}
}

TEST_CASE("embed: attach hands back a handle with the engine's geometry and a working client", "[embed]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    CHECK(e.st == CLOCKWORK_OK);
    CHECK(clockwork_embed_sample_rate(e.h) == lanes_test::kSampleRate);
    ClockworkClient* c = clockwork_embed_client(e.h);
    REQUIRE(c != nullptr);
    ClockworkClientInfo info {};
    info.struct_bytes = sizeof info;
    REQUIRE(clockwork_client_info(c, &info) == CLOCKWORK_OK);
    CHECK(clockwork_embed_block_size(e.h) == info.block_frames);

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
    CHECK(takeReply([c](ClockworkClientMessage* m, uint32_t max) { return clockwork_client_poll(c, m, max); },
                    0x5a5a, "/dummy/pong"));
}

// Engines side by side in one process (docs/ENGINES.md.2): each
// handle is an engine of its own, with its own memory, client and stream,
// and closing one leaves the other running.
TEST_CASE("embed: two attaches are two engines, each its own, and closing one leaves the other", "[embed]") {
    Handle a, b;
    REQUIRE(a.h != nullptr);
    REQUIRE(b.h != nullptr);
    CHECK(b.st == CLOCKWORK_OK);
    ClockworkClient* ca = clockwork_embed_client(a.h);
    ClockworkClient* cb = clockwork_embed_client(b.h);
    const auto pollA = [ca](ClockworkClientMessage* m, uint32_t max) { return clockwork_client_poll(ca, m, max); };
    const auto pollB = [cb](ClockworkClientMessage* m, uint32_t max) { return clockwork_client_poll(cb, m, max); };
    ClockworkRegion ra {}, rb {};
    REQUIRE(clockwork_client_region(ca, CLOCKWORK_REGION_SCOPE, &ra) == CLOCKWORK_OK);
    REQUIRE(clockwork_client_region(cb, CLOCKWORK_REGION_SCOPE, &rb) == CLOCKWORK_OK);
    CHECK(ra.base != rb.base);

    // Each engine answers its own client, and only its own.
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(ca, ping.ptr(), ping.size(), 0xa1) == CLOCKWORK_OK);
    REQUIRE(clockwork_client_send(cb, ping.ptr(), ping.size(), 0xb2) == CLOCKWORK_OK);
    std::vector<float> al(64), ar(64), bl(64), br(64);
    float* outA[2] = { al.data(), ar.data() };
    float* outB[2] = { bl.data(), br.data() };
    REQUIRE(clockwork_embed_render(a.h, outA, 2, nullptr, 0, 64) == 64);
    REQUIRE(clockwork_embed_render(b.h, outB, 2, nullptr, 0, 64) == 64);
    CHECK(takeReply(pollA, 0xa1, "/dummy/pong"));
    CHECK(takeReply(pollB, 0xb2, "/dummy/pong"));
    // The same frames of the same guest: the same audio, each engine's own.
    CHECK(al == bl);
    CHECK(ar == br);

    clockwork_embed_close(a.h);
    a.h = nullptr;
    REQUIRE(clockwork_client_send(cb, ping.ptr(), ping.size(), 0xb3) == CLOCKWORK_OK);
    REQUIRE(clockwork_embed_render(b.h, outB, 2, nullptr, 0, 64) == 64);
    CHECK(takeReply(pollB, 0xb3, "/dummy/pong"));
}

// A guest that says any (DSP_INSTANCES_ANY) promises it keeps nothing
// process-wide (docs/BOUNDARY.md, "How many in a process"), and this holds
// the dummy to it: two instances at once, each told something different at
// different blocks, render bit for bit what each renders alone. State the two
// shared — a frame count, a mode, a phase — shows in one or both. A guest
// that says any owes the same case in its own suite.
namespace {
// A message sent before the block given.
struct Cue { uint32_t block; osc_test::Packet message; };
using Script = std::vector<Cue>;
constexpr uint32_t kScriptBlocks = 400;

// One engine running a script, block by block, its output kept.
struct Run {
    Handle e;
    const Script& script;
    std::vector<float> l, r;
    explicit Run(const Script& s) : script(s) { REQUIRE(e.h != nullptr); }
    void block(uint32_t b) {
        for (const Cue& c : script)
            if (c.block == b)
                REQUIRE(clockwork_client_send(clockwork_embed_client(e.h), c.message.ptr(), c.message.size(), 0)
                        == CLOCKWORK_OK);
        const uint32_t n = clockwork_embed_block_size(e.h);
        const size_t at = l.size();
        l.resize(at + n);
        r.resize(at + n);
        float* out[2] = { l.data() + at, r.data() + at };
        REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, n) == n);
    }
};

struct Output { std::vector<float> l, r; };
Output alone(const Script& s) {
    Run run(s);
    for (uint32_t b = 0; b < kScriptBlocks; ++b) run.block(b);
    return { run.l, run.r };
}

// The first frame where `got` is not `want`, or -1 when it is the same.
long firstDifference(const std::vector<float>& got, const std::vector<float>& want) {
    if (got.size() != want.size()) return static_cast<long>(std::min(got.size(), want.size()));
    const auto at = std::mismatch(got.begin(), got.end(), want.begin());
    return at.first == got.end() ? -1 : static_cast<long>(at.first - got.begin());
}
}

TEST_CASE("embed: a guest that says any renders beside another exactly what it renders alone", "[embed][guest]") {
    REQUIRE(dsp_describe()->instances == DSP_INSTANCES_ANY);
    const Script a = { { 0, osc_test::message("/dummy/pulse", 2, 7) },
                       { 150, osc_test::message("/dummy/tone") } };
    const Script b = { { 0, osc_test::message("/dummy/tone") },
                       { 90, osc_test::message("/dummy/pulse", 5, 11) },
                       { 260, osc_test::message("/dummy/pulse", 1, 3) } };
    const Output soloA = alone(a), soloB = alone(b);
    // Two scripts that sound different, so the case can tell them apart.
    REQUIRE(firstDifference(soloA.l, soloB.l) >= 0);
    const auto peak = [](const std::vector<float>& v) {
        float p = 0.0f;
        for (float x : v) p = std::max(p, std::fabs(x));
        return p;
    };
    REQUIRE(peak(soloA.l) > 0.0f);
    REQUIRE(peak(soloB.l) > 0.0f);

    Run ra(a), rb(b);
    for (uint32_t blk = 0; blk < kScriptBlocks; ++blk) {
        ra.block(blk);
        rb.block(blk);
    }
    CHECK(firstDifference(ra.l, soloA.l) == -1);
    CHECK(firstDifference(ra.r, soloA.r) == -1);
    CHECK(firstDifference(rb.l, soloB.l) == -1);
    CHECK(firstDifference(rb.r, soloB.r) == -1);
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
    CHECK(takeReply([out](ClockworkClientMessage* msgs, uint32_t max) { return clockwork_client_tap_poll(out, msgs, max); },
                    origin, "/dummy/pong"));
    CHECK(clockwork_client_tap_missed(in) + clockwork_client_tap_missed(out) == 0);
    // Watching took nothing: the consumer still gets the reply.
    CHECK(takeReply([c](ClockworkClientMessage* msgs, uint32_t max) { return clockwork_client_poll(c, msgs, max); },
                    origin, "/dummy/pong"));

    clockwork_client_tap_close(in);
    clockwork_client_tap_close(out);
}

TEST_CASE("embed: render answers any buffer size with exactly the engine's output stream", "[embed]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    // The dummy's pulse is on by default; whatever it renders, the OUT slot
    // holds the stream block by block, and render must hand back that
    // stream cut at the host's sizes — no frame dropped, doubled or shifted.
    shm_audio_buffer_reader tap = scopeReader(e.h, CLOCKWORK_TAP_OUT);
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
    // Everything the taps saw, interleaved: at least as many frames as served.
    std::vector<float> stream(gotL.size() * 2 + clockwork_embed_block_size(e.h) * 2);
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
    // Constant input on both channels; after a few blocks the IN slot's newest
    // block is that constant (the first block may be padded with silence).
    shm_audio_buffer_reader reader = scopeReader(e.h, CLOCKWORK_TAP_IN);
    reader.seek_to_live();
    std::vector<float> a(block * 3, 0.25f), b(block * 3, -0.5f);
    const float* in[2] = { a.data(), b.data() };
    std::vector<float> l(block * 3), r(block * 3);
    float* out[2] = { l.data(), r.data() };
    REQUIRE(clockwork_embed_render(e.h, out, 2, in, 2, block * 3) == block * 3);
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
}
