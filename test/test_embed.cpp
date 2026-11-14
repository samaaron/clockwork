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
#include "clockwork_clock_state.h"
#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"
#include "core_state.h"
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

// The assets area (docs/MEMORY.md): where a client stages bulk for the guest,
// a sample above all, outside the arena and provided by whoever boots the
// engine. An attached engine is booted by its handle, so the handle provides
// it, as a device boot does: a plugin loads samples like any host.
namespace {
osc_test::Packet assetCommit(int32_t id, int32_t offset, int32_t bytes) {
    osc_test::Builder b;
    auto& s = b.begin("/clockwork/asset/commit");
    s << id << int32_t(CLOCKWORK_ASSET_RAW) << offset << bytes << int32_t(0) << int32_t(0) << 0.0f;
    return b.end();
}
uint32_t fnv(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}
// Renders until the client hears `address` answering `origin`, or a second of blocks has gone by.
bool renderUntil(ClockworkEmbed* h, uint32_t origin, const std::string& address, std::vector<int32_t>* args = nullptr) {
    ClockworkClient* c = clockwork_embed_client(h);
    std::vector<float> l(256), r(256);
    float* out[2] = { l.data(), r.data() };
    for (int i = 0; i < 200; ++i) {
        clockwork_embed_render(h, out, 2, nullptr, 0, 256);
        ClockworkClientMessage m[64];
        for (uint32_t n; (n = clockwork_client_poll(c, m, 64)) > 0;)
            for (uint32_t k = 0; k < n; ++k)
                if (m[k].origin == origin && osc_test::parseAddress(m[k].bytes, m[k].length) == address) {
                    if (args) {
                        const osc_test::ParsedReply p = osc_test::parseReply(m[k].bytes, m[k].length);
                        for (int a = 0; a < p.argCount(); ++a) args->push_back(p.argInt(a));
                    }
                    return true;
                }
    }
    return false;
}
}

TEST_CASE("embed: an attached engine has an assets area, and a sample staged there reaches the guest", "[embed][asset]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkClient* c = clockwork_embed_client(e.h);
    ClockworkRegion assets {};
    REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_ASSETS, &assets) == CLOCKWORK_OK);
    REQUIRE(assets.base != nullptr);
    REQUIRE(assets.bytes >= 65536);

    // the client's bytes, where it chose to put them; the guest reads these very bytes
    auto* lane = static_cast<uint8_t*>(assets.base);
    const uint32_t off = 4096, n = 1000;
    for (uint32_t i = 0; i < n; ++i) lane[off + i] = static_cast<uint8_t>((i * 7) ^ 0x5A);
    const auto commit = assetCommit(7, int32_t(off), int32_t(n));
    REQUIRE(clockwork_client_send(c, commit.ptr(), commit.size(), 0x7711) == CLOCKWORK_OK);
    CHECK(renderUntil(e.h, 0x7711, "/clockwork/asset/committed"));

    const auto last = osc_test::message("/dummy/asset/last");
    REQUIRE(clockwork_client_send(c, last.ptr(), last.size(), 0x7712) == CLOCKWORK_OK);
    std::vector<int32_t> got;
    REQUIRE(renderUntil(e.h, 0x7712, "/dummy/asset/last.reply", &got));
    REQUIRE(got.size() == 7);
    CHECK(got[0] == 7);
    CHECK(got[2] == int32_t(n));
    CHECK(static_cast<uint32_t>(got[6]) == fnv(lane + off, n));
}

TEST_CASE("embed: the assets area is the size the host asks for, and stays as attached through a reconfigure", "[embed][asset]") {
    ClockworkEmbedConfig cfg = config();
    cfg.assets_bytes = 3u * 1024u * 1024u;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkEmbed* h = clockwork_embed_attach(&cfg, &st);
    REQUIRE(h != nullptr);
    ClockworkRegion assets {};
    REQUIRE(clockwork_client_region(clockwork_embed_client(h), CLOCKWORK_REGION_ASSETS, &assets) == CLOCKWORK_OK);
    CHECK(assets.bytes == cfg.assets_bytes);
    const void* base = assets.base;

    // a host's deactivate and activate: what a client staged is where it was
    static_cast<uint8_t*>(assets.base)[100] = 0x3C;
    ClockworkEmbedConfig again = cfg;
    again.assets_bytes = 1024;   // not read: the area stays as attached
    REQUIRE(clockwork_embed_reconfigure(h, &again) == CLOCKWORK_OK);
    ClockworkRegion after {};
    REQUIRE(clockwork_client_region(clockwork_embed_client(h), CLOCKWORK_REGION_ASSETS, &after) == CLOCKWORK_OK);
    CHECK(after.base == base);
    CHECK(after.bytes == cfg.assets_bytes);
    CHECK(static_cast<uint8_t*>(after.base)[100] == 0x3C);
    clockwork_embed_close(h);
}

TEST_CASE("embed: the guest's options reach the guest as the host wrote them; ones too long are refused", "[embed]") {
    // the dummy guest reports the first four words of what it was given (/dummy/region/config)
    const char text[] = "abcdefghijklmnop qrstuvwxyz";
    ClockworkEmbedConfig cfg = config();
    cfg.guest_config = text;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkEmbed* h = clockwork_embed_attach(&cfg, &st);
    REQUIRE(h != nullptr);
    const auto ask = osc_test::message("/dummy/region/config");
    REQUIRE(clockwork_client_send(clockwork_embed_client(h), ask.ptr(), ask.size(), 0x7713) == CLOCKWORK_OK);
    std::vector<int32_t> got;
    REQUIRE(renderUntil(h, 0x7713, "/dummy/region/config-is", &got));
    REQUIRE(got.size() == 5);
    CHECK(static_cast<uint32_t>(got[0]) >= sizeof text);   // the region holds it, NUL and all
    uint32_t words[4];
    std::memcpy(words, text, sizeof words);
    for (int i = 0; i < 4; ++i) CHECK(static_cast<uint32_t>(got[1 + i]) == words[i]);
    clockwork_embed_close(h);

    const std::string tooLong(GUEST_CONFIG_SIZE, 'x');
    cfg.guest_config = tooLong.c_str();
    CHECK(clockwork_embed_attach(&cfg, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ARG);
}

// ── A host's transport, said to the timeline as a Link peer's is ───────────

namespace {
const ClockworkClockState* clockOf(ClockworkEmbed* h) {
    ClockworkCore* core = clockwork_client_core(clockwork_embed_client(h));
    const ClockworkArenaEntry* e = clockwork_arena_find(clockwork_arena_header(core), CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE);
    REQUIRE(e != nullptr);
    return reinterpret_cast<const ClockworkClockState*>(static_cast<const uint8_t*>(clockwork_lanes_base(core)) + e->offset);
}

// The engine's time for the last block's first frame.
double lastBlockNtp(ClockworkEmbed* h) {
    const ClockworkTimetag t = clockwork_block_time(clockwork_client_core(clockwork_embed_client(h)));
    return static_cast<double>(t.ntp) / 4294967296.0;
}

// What a host says before a render, as a plugin reads it from its host's
// transport: what it leaves at these defaults is not said.
struct Said {
    double   bpm = 0.0;
    int32_t  num = 0, den = 0;
    int      playing = -1;
    bool     placed = false;
    double   beat = 0.0;
    uint32_t how = CLOCKWORK_BEAT_FORCE;
};
void say(ClockworkEmbed* h, const Said& t) {
    if (t.bpm != 0.0) REQUIRE(clockwork_embed_tempo(h, t.bpm, 0) == CLOCKWORK_OK);
    if (t.num != 0) REQUIRE(clockwork_embed_meter(h, t.num, t.den, 0) == CLOCKWORK_OK);
    if (t.playing >= 0) REQUIRE(clockwork_embed_playing(h, t.playing, 0) == CLOCKWORK_OK);
    if (t.placed) REQUIRE(clockwork_embed_beat(h, t.beat, t.how, 0) == CLOCKWORK_OK);
}
// A host whose beat number is the timeline's: a step sequencer in a DAW.
Said beats(double bpm, double beat, bool playing = true) {
    Said t;
    t.bpm = bpm;
    t.playing = playing ? 1 : 0;
    t.placed = true;
    t.beat = beat;
    return t;
}

void renderFrames(ClockworkEmbed* h, uint32_t frames) {
    std::vector<float> l(frames), r(frames);
    float* out[2] = { l.data(), r.data() };
    REQUIRE(clockwork_embed_render(h, out, 2, nullptr, 0, frames) == frames);
}

// Rendering at the host's pace, its beat line continuous, so the grid stays put
// and any block's beat can be read back.
struct Host {
    ClockworkEmbed* h;
    double rate, bpm, start;
    uint64_t frames = 0;
    double beatNow() const { return start + static_cast<double>(frames) / rate * bpm / 60.0; }
    void buffer(uint32_t n) {
        say(h, beats(bpm, beatNow()));
        renderFrames(h, n);
        frames += n;
    }
};

// Everything the handle's client was sent since it was last asked.
std::vector<osc_test::ParsedReply> heardBy(ClockworkEmbed* h) {
    std::vector<osc_test::ParsedReply> heard;
    ClockworkClient* c = clockwork_embed_client(h);
    ClockworkClientMessage m[64];
    for (uint32_t n; (n = clockwork_client_poll(c, m, 64)) > 0;)
        for (uint32_t k = 0; k < n; ++k) heard.push_back(osc_test::parseReply(m[k].bytes, m[k].length));
    return heard;
}
void send(ClockworkEmbed* h, const osc_test::Packet& p) {
    REQUIRE(clockwork_client_send(clockwork_embed_client(h), p.ptr(), p.size(), 1) == CLOCKWORK_OK);
}
}

TEST_CASE("embed: a host's beat lands on the frame it names, whatever the host's buffers", "[embed][timeline]") {
    for (uint32_t buffer : { 37u, 128u, 256u, 512u, 1000u }) {
        INFO("a buffer of " << buffer);
        Handle e;
        REQUIRE(e.h != nullptr);
        ClockworkEmbed* h = e.h;
        const double rate = clockwork_embed_sample_rate(h);
        const uint32_t block = clockwork_embed_block_size(h);
        const ClockworkClockState* clock = clockOf(h);
        Host host{ h, rate, 120.0, 8.0 };
        // Long enough to wrap the FIFO many times; checked from the first block.
        for (int i = 0; i < 200; ++i) {
            host.buffer(buffer);
            // The first frame of the last block ticked, on the host's own line.
            const uint64_t ticks = (host.frames + block - 1) / block;
            const uint64_t first = (ticks - 1) * block;
            const double want = host.start + static_cast<double>(first) / rate * host.bpm / 60.0;
            const ClockworkClockSnapshot c = readClockworkClock(clock);
            const double got = clockwork::beatAt(lastBlockNtp(h), c.beat_origin_ntp, c.bpm);
            const double oneFrame = host.bpm / 60.0 / rate;
            INFO("buffer index " << i << ": beat " << got << ", wanted " << want);
            REQUIRE(std::abs(got - want) < oneFrame);
        }
    }
}

TEST_CASE("embed: a beat said for a frame inside the render lands on that frame", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    const double rate = clockwork_embed_sample_rate(e.h);
    const uint32_t block = clockwork_embed_block_size(e.h);
    renderFrames(e.h, 512);
    REQUIRE(clockwork_embed_tempo(e.h, 120.0, 0) == CLOCKWORK_OK);
    REQUIRE(clockwork_embed_playing(e.h, 1, 0) == CLOCKWORK_OK);
    // Beat 16 at the 300th frame of the next render: frame 812 of the host's count.
    REQUIRE(clockwork_embed_beat(e.h, 16.0, CLOCKWORK_BEAT_FORCE, 300) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    const uint64_t first = 1024 - block;   // the last block's first frame
    const double want = 16.0 + (static_cast<double>(first) - 812.0) / rate * 2.0;
    const ClockworkClockSnapshot c = readClockworkClock(clockOf(e.h));
    CHECK(std::abs(clockwork::beatAt(lastBlockNtp(e.h), c.beat_origin_ntp, c.bpm) - want) < 2.0 / rate);
}

TEST_CASE("embed: the timeline is at its default until its host speaks, and follows it from then on", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    const ClockworkClockState* clock = clockOf(e.h);
    // A host with no transport says nothing: the timeline starts as any session's does, at its default.
    const ClockworkClockSnapshot fresh = readClockworkClock(clock);
    CHECK((fresh.flags & SC_FLAG_HOST_TIMELINE) == 0);
    for (int i = 0; i < 8; ++i) renderFrames(e.h, 512);
    ClockworkClockSnapshot c = readClockworkClock(clock);
    CHECK((c.flags & SC_FLAG_HOST_TIMELINE) == 0);
    CHECK(c.bpm == fresh.bpm);

    // It speaks: the timeline follows, and the clock says a host has said its transport.
    Said t = beats(90.0, 4.0, true);
    t.num = 7;
    t.den = 8;
    say(e.h, t);
    renderFrames(e.h, 512);
    c = readClockworkClock(clock);
    CHECK(c.bpm == 90.0);
    CHECK(c.is_playing);
    CHECK(c.meter_num == 7);
    CHECK(c.meter_den == 8);
    CHECK((c.flags & SC_FLAG_HOST_TIMELINE) != 0);

    // Stopped: the clock says so.
    t.playing = 0;
    say(e.h, t);
    renderFrames(e.h, 512);
    c = readClockworkClock(clock);
    CHECK_FALSE(c.is_playing);
    CHECK((c.flags & SC_FLAG_HOST_TIMELINE) != 0);
    CHECK(c.meter_num == 7);

    // A host that then says nothing more leaves it there.
    for (int i = 0; i < 8; ++i) renderFrames(e.h, 512);
    c = readClockworkClock(clock);
    CHECK(c.bpm == 90.0);
    CHECK((c.flags & SC_FLAG_HOST_TIMELINE) != 0);
}

TEST_CASE("embed: one thing said is enough for the clock to say a host has a transport", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    renderFrames(e.h, 512);
    REQUIRE(clockwork_embed_playing(e.h, 0, 0) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    CHECK((readClockworkClock(clockOf(e.h)).flags & SC_FLAG_HOST_TIMELINE) != 0);
}

// The host is a source like a Link peer: it wins by saying its transport again, not by a rule.
TEST_CASE("embed: what another sets lasts until the host next says its own, and what the host never says is left",
          "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkClock* engineClock = clockwork::clock_of(clockwork_client_core(clockwork_embed_client(e.h)));
    REQUIRE(engineClock != nullptr);
    const ClockworkClockState* clock = clockOf(e.h);
    // A host that says its tempo and nothing of its time signature.
    REQUIRE(clockwork_embed_tempo(e.h, 90.0, 0) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    REQUIRE(readClockworkClock(clock).bpm == 90.0);

    // Another source sets both, as a client or a Link peer would.
    engineClock->setBpm(140.0);
    engineClock->setMeter(3, 4);
    CHECK(readClockworkClock(clock).bpm == 140.0);

    REQUIRE(clockwork_embed_tempo(e.h, 90.0, 0) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    const ClockworkClockSnapshot c = readClockworkClock(clock);
    CHECK(c.bpm == 90.0);
    CHECK(c.meter_num == 3);
    CHECK(c.meter_den == 4);
}

// The clock's commands are the core's to answer, on every host: a plugin's
// client asks and sets as a native or a web client does.
namespace {
// The client asks for the timeline's notices, as it would by command.
void wantClock(ClockworkEmbed* h) {
    clockwork_client_wants(clockwork_client_core(clockwork_embed_client(h)), CLOCKWORK_WANTS_CLOCK, true);
}
osc_test::Packet tempoSet(float bpm) {
    osc_test::Builder b;
    b.begin("/clockwork/clock/tempo/set") << bpm;
    return b.end();
}
}

TEST_CASE("embed: an attached core answers the clock's commands", "[embed][timeline][commands]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    renderFrames(e.h, 512);
    heardBy(e.h);
    const ClockworkClockSnapshot was = readClockworkClock(clockOf(e.h));

    send(e.h, osc_test::message("/clockwork/clock/tempo/get", 5));
    send(e.h, osc_test::message("/clockwork/clock/meter"));
    send(e.h, osc_test::message("/clockwork/clock/transport/get"));
    send(e.h, osc_test::message("/clockwork/clock/visibility/get"));   // Link's own: not the core's
    renderFrames(e.h, 512);
    bool tempo = false, meter = false, transport = false, unsupported = false;
    for (const auto& r : heardBy(e.h)) {
        CHECK(r.address != "/clockwork/error");
        if (r.address == "/clockwork/clock/tempo.reply") {
            tempo = true;
            CHECK(r.argDouble(0) == was.bpm);
            CHECK(r.argInt(1) == 5);
        } else if (r.address == "/clockwork/clock/meter.reply") {
            meter = true;
            CHECK(r.argInt(0) == was.meter_num);
            CHECK(r.argInt(1) == was.meter_den);
        } else if (r.address == "/clockwork/clock/transport.reply") {
            transport = true;
            CHECK(r.argInt(0) == 0);
        } else if (r.address == "/clockwork/clock/unsupported") {
            unsupported = true;
            CHECK(r.argString(0) == "/clockwork/clock/visibility/get");
        }
    }
    CHECK(tempo);
    CHECK(meter);
    CHECK(transport);
    CHECK(unsupported);
}

TEST_CASE("embed: a client sets the timeline of an attached core, and is told as of any source's move",
          "[embed][timeline][commands]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    wantClock(e.h);
    renderFrames(e.h, 512);
    heardBy(e.h);

    send(e.h, tempoSet(97.0f));
    send(e.h, osc_test::message("/clockwork/clock/meter", 3, 4));
    send(e.h, osc_test::message("/clockwork/clock/transport/set", 1));
    for (int i = 0; i < 4; ++i) renderFrames(e.h, 512);
    const ClockworkClockSnapshot c = readClockworkClock(clockOf(e.h));
    CHECK(c.bpm == 97.0);
    CHECK(c.meter_num == 3);
    CHECK(c.meter_den == 4);
    CHECK(c.is_playing);
    // No host has said a transport: the clock does not say one has.
    CHECK((c.flags & SC_FLAG_HOST_TIMELINE) == 0);

    std::vector<double> tempos;
    std::vector<int32_t> playing;
    for (const auto& r : heardBy(e.h)) {
        CHECK(r.address != "/clockwork/error");
        CHECK(r.address != "/clockwork/clock/unsupported");
        if (r.address == "/clockwork/clock/notify/tempo") tempos.push_back(r.argDouble(0));
        else if (r.address == "/clockwork/clock/notify/transport") playing.push_back(r.argInt(0));
    }
    REQUIRE_FALSE(tempos.empty());
    CHECK(tempos.back() == 97.0);
    REQUIRE(playing.size() == 1);
    CHECK(playing[0] == 1);
}

TEST_CASE("embed: what a client sets by command lasts until the host next says its own", "[embed][timeline][commands]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    REQUIRE(clockwork_embed_tempo(e.h, 90.0, 0) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    REQUIRE(readClockworkClock(clockOf(e.h)).bpm == 90.0);

    send(e.h, tempoSet(140.0f));
    renderFrames(e.h, 512);
    CHECK(readClockworkClock(clockOf(e.h)).bpm == 140.0);

    REQUIRE(clockwork_embed_tempo(e.h, 90.0, 0) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    CHECK(readClockworkClock(clockOf(e.h)).bpm == 90.0);
}

// ── What the client asked to be told (lanes.h, clockwork_client_wants) ─────
// One client, so one switch for each kind of notice, kept by the core and the
// same on every host. A notice is written only when its switch is on.

TEST_CASE("embed: the client's switches are off until it asks, and off again when it has gone", "[embed][wants]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkCore* core = clockwork_client_core(clockwork_embed_client(e.h));
    CHECK(clockwork_client_wanted(core) == 0u);
    CHECK(clockwork_client_wants(core, CLOCKWORK_WANTS_MIDI, true));         // newly on
    CHECK_FALSE(clockwork_client_wants(core, CLOCKWORK_WANTS_MIDI, true));   // on already
    CHECK(clockwork_client_wants(core, CLOCKWORK_WANTS_CLOCK | CLOCKWORK_WANTS_OSC, true));
    CHECK(clockwork_client_wanted(core) == (CLOCKWORK_WANTS_MIDI | CLOCKWORK_WANTS_CLOCK | CLOCKWORK_WANTS_OSC));
    CHECK(clockwork_client_wants(core, CLOCKWORK_WANTS_MIDI, false));
    CHECK_FALSE(clockwork_client_wants(core, CLOCKWORK_WANTS_MIDI, false));
    CHECK(clockwork_client_wanted(core) == (CLOCKWORK_WANTS_CLOCK | CLOCKWORK_WANTS_OSC));
    // A kind there is none of is no switch.
    CHECK_FALSE(clockwork_client_wants(core, 1u << 20u, true));
    CHECK(clockwork_client_wanted(core) == (CLOCKWORK_WANTS_CLOCK | CLOCKWORK_WANTS_OSC));
    // The client went: the next one starts with nothing asked for.
    clockwork_client_gone(core);
    CHECK(clockwork_client_wanted(core) == 0u);
    CHECK_FALSE(clockwork_client_wants(nullptr, CLOCKWORK_WANTS_MIDI, true));
    CHECK(clockwork_client_wanted(nullptr) == 0u);
}

TEST_CASE("embed: the timeline's notices are sent once the client has asked by command, and not before",
          "[embed][wants][commands]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    renderFrames(e.h, 512);
    heardBy(e.h);
    const auto tempos = [&] {
        std::vector<double> t;
        for (const auto& r : heardBy(e.h))
            if (r.address == "/clockwork/clock/notify/tempo") t.push_back(r.argDouble(0));
        return t;
    };

    // Unasked: the tempo moves and nothing is said.
    REQUIRE(clockwork_embed_tempo(e.h, 90.0, 0) == CLOCKWORK_OK);
    for (int i = 0; i < 4; ++i) renderFrames(e.h, 512);
    REQUIRE(readClockworkClock(clockOf(e.h)).bpm == 90.0);
    CHECK(tempos().empty());

    // It asks, with a token: acked, and told where the timeline is now.
    send(e.h, osc_test::message("/clockwork/clock/notify/subscribe", 7));
    renderFrames(e.h, 512);
    bool acked = false;
    std::vector<double> told;
    for (const auto& r : heardBy(e.h)) {
        CHECK(r.address != "/clockwork/clock/unsupported");
        if (r.address == "/clockwork/clock/notify/subscribe.reply") { acked = true; CHECK(r.argInt(0) == 7); }
        else if (r.address == "/clockwork/clock/notify/tempo") told.push_back(r.argDouble(0));
    }
    CHECK(acked);
    REQUIRE(told.size() == 1);
    CHECK(told[0] == 90.0);
    CHECK((clockwork_client_wanted(clockwork_client_core(clockwork_embed_client(e.h))) & CLOCKWORK_WANTS_CLOCK) != 0u);

    // From then on it hears each move.
    REQUIRE(clockwork_embed_tempo(e.h, 132.0, 0) == CLOCKWORK_OK);
    for (int i = 0; i < 4; ++i) renderFrames(e.h, 512);
    const std::vector<double> after = tempos();
    REQUIRE_FALSE(after.empty());
    CHECK(after.back() == 132.0);

    // And no more once it says so.
    send(e.h, osc_test::message("/clockwork/clock/notify/unsubscribe"));
    renderFrames(e.h, 512);
    heardBy(e.h);
    REQUIRE(clockwork_embed_tempo(e.h, 75.0, 0) == CLOCKWORK_OK);
    for (int i = 0; i < 4; ++i) renderFrames(e.h, 512);
    CHECK(tempos().empty());
}

TEST_CASE("embed: what a port or a controller sent reaches the client only once it has asked", "[embed][wants]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkCore* core = clockwork_client_core(clockwork_embed_client(e.h));
    renderFrames(e.h, 512);
    heardBy(e.h);
    const auto heard = [&](const char* address) {
        int n = 0;
        for (const auto& r : heardBy(e.h)) n += r.address == address;
        return n;
    };
    send(e.h, osc_test::message("/clockwork/midi/in/note_on", 60, 100, 1));
    send(e.h, osc_test::message("/clockwork/gamepad/in/button", 0, 1));
    renderFrames(e.h, 512);
    CHECK(heard("/clockwork/midi/in/note_on") == 0);

    // Asked for by command: the switch is the core's, whoever answers the verb's own question.
    send(e.h, osc_test::message("/clockwork/midi/notify/subscribe"));
    renderFrames(e.h, 512);
    heardBy(e.h);
    CHECK((clockwork_client_wanted(core) & CLOCKWORK_WANTS_MIDI) != 0u);
    CHECK((clockwork_client_wanted(core) & CLOCKWORK_WANTS_GAMEPAD) == 0u);

    send(e.h, osc_test::message("/clockwork/midi/in/note_on", 60, 100, 1));
    send(e.h, osc_test::message("/clockwork/gamepad/in/button", 0, 1));
    renderFrames(e.h, 512);
    int midi = 0, pad = 0;
    for (const auto& r : heardBy(e.h)) {
        midi += r.address == "/clockwork/midi/in/note_on";
        pad += r.address == "/clockwork/gamepad/in/button";
    }
    CHECK(midi == 1);
    CHECK(pad == 0);

    send(e.h, osc_test::message("/clockwork/midi/notify/unsubscribe"));
    renderFrames(e.h, 512);
    CHECK((clockwork_client_wanted(core) & CLOCKWORK_WANTS_MIDI) == 0u);
}

TEST_CASE("embed: the grid moves when a host's line does, and not while it is steady", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkEmbed* h = e.h;
    const ClockworkClockState* clock = clockOf(h);
    // Time from samples alone: this renders far faster than the wall clock, which
    // the engine's time is otherwise steered toward (the case above covers that).
    ClockworkClock* engineClock = clockwork::clock_of(clockwork_client_core(clockwork_embed_client(h)));
    REQUIRE(engineClock != nullptr);
    engineClock->setFreewheelClock(true, 3900000000.0);
    Host host{ h, clockwork_embed_sample_rate(h), 120.0, 0.0 };
    for (int i = 0; i < 4; ++i) host.buffer(300);
    const uint32_t steady = readClockworkClock(clock).generation;
    for (int i = 0; i < 400; ++i) host.buffer(300);
    CHECK(readClockworkClock(clock).generation == steady);

    // A new tempo: the beat playing now holds.
    const double before = host.beatNow();
    host.start = before;
    host.frames = 0;
    host.bpm = 150.0;
    host.buffer(300);
    CHECK(readClockworkClock(clock).generation != steady);
    CHECK(readClockworkClock(clock).bpm == 150.0);

    // A loop back: the beat jumps, and the grid with it.
    const uint32_t beforeJump = readClockworkClock(clock).generation;
    host.start = 0.0;
    host.frames = 0;
    host.buffer(300);
    CHECK(readClockworkClock(clock).generation != beforeJump);
    const ClockworkClockSnapshot c = readClockworkClock(clock);
    CHECK(clockwork::beatAt(lastBlockNtp(h), c.beat_origin_ntp, c.bpm) < 0.1);
}

TEST_CASE("embed: a stopped host's grid stands still, follows its tempo, and lands where it starts", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    ClockworkEmbed* h = e.h;
    const ClockworkClockState* clock = clockOf(h);
    ClockworkClock* engineClock = clockwork::clock_of(clockwork_client_core(clockwork_embed_client(h)));
    REQUIRE(engineClock != nullptr);
    engineClock->setFreewheelClock(true, 3900000000.0);
    // Its beat stands still while time goes on, and it says so on every buffer.
    Said t = beats(120.0, 4.0, false);
    const auto buffers = [&](int n) {
        for (int i = 0; i < n; ++i) {
            say(h, t);
            renderFrames(h, 256);
        }
    };
    buffers(4);
    const uint32_t steady = readClockworkClock(clock).generation;
    buffers(200);
    CHECK(readClockworkClock(clock).generation == steady);
    CHECK_FALSE(readClockworkClock(clock).is_playing);

    t.bpm = 90.0;
    buffers(1);
    CHECK(readClockworkClock(clock).bpm == 90.0);
    CHECK(readClockworkClock(clock).generation != steady);

    t.playing = 1;
    buffers(1);
    const ClockworkClockSnapshot c = readClockworkClock(clock);
    CHECK(c.is_playing);
    // The buffer's frames are rendered: this is the last block's first frame.
    const double frames = clockwork_block_size(clockwork_client_core(clockwork_embed_client(h)));
    const double intoBuffer = 256.0 - frames;
    const double want = 4.0 + intoBuffer / clockwork_embed_sample_rate(h) * 90.0 / 60.0;
    CHECK(std::fabs(clockwork::beatAt(lastBlockNtp(h), c.beat_origin_ntp, c.bpm) - want) < 0.001);
}

TEST_CASE("embed: what a host says that the timeline cannot take is refused and changes nothing", "[embed][timeline]") {
    CHECK(clockwork_embed_tempo(nullptr, 120.0, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_meter(nullptr, 4, 4, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_playing(nullptr, 1, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_beat(nullptr, 0.0, CLOCKWORK_BEAT_FORCE, 0) == CLOCKWORK_E_ARG);
    Handle e;
    REQUIRE(e.h != nullptr);
    renderFrames(e.h, 512);
    const ClockworkClockSnapshot was = readClockworkClock(clockOf(e.h));
    for (double bad : { 0.0, -1.0, std::nan(""), HUGE_VAL }) CHECK(clockwork_embed_tempo(e.h, bad, 0) == CLOCKWORK_E_ARG);
    for (double bad : { std::nan(""), HUGE_VAL }) CHECK(clockwork_embed_beat(e.h, bad, CLOCKWORK_BEAT_FORCE, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_beat(e.h, 4.0, 2u, 0) == CLOCKWORK_E_ARG);   // neither a request nor a force
    CHECK(clockwork_embed_meter(e.h, 7, 0, 0) == CLOCKWORK_E_ARG);
    renderFrames(e.h, 512);
    const ClockworkClockSnapshot now = readClockworkClock(clockOf(e.h));
    CHECK(now.bpm == was.bpm);
    CHECK(now.generation == was.generation);
}

TEST_CASE("embed: said twice before a render, the later is the one", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    renderFrames(e.h, 512);
    REQUIRE(clockwork_embed_tempo(e.h, 90.0, 0) == CLOCKWORK_OK);
    REQUIRE(clockwork_embed_tempo(e.h, 133.0, 0) == CLOCKWORK_OK);
    renderFrames(e.h, 512);
    CHECK(readClockworkClock(clockOf(e.h)).bpm == 133.0);
}

TEST_CASE("embed: two engines keep two hosts' timelines", "[embed][timeline]") {
    Handle a, b;
    REQUIRE(a.h != nullptr);
    REQUIRE(b.h != nullptr);
    renderFrames(a.h, 512);
    renderFrames(b.h, 512);
    const double was = readClockworkClock(clockOf(b.h)).bpm;
    say(a.h, beats(90.0, 1.0));
    renderFrames(a.h, 512);
    renderFrames(b.h, 512);
    CHECK(readClockworkClock(clockOf(a.h)).bpm == 90.0);
    CHECK(readClockworkClock(clockOf(a.h)).is_playing);
    CHECK(readClockworkClock(clockOf(b.h)).bpm == was);
    CHECK_FALSE(readClockworkClock(clockOf(b.h)).is_playing);
}

// What a guest reads through DspConfig::clock is that same state: the dummy
// guest reports the tempo and transport it sees.
TEST_CASE("embed: a guest reads a host's timeline through its clock", "[embed][timeline]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    renderFrames(e.h, 512);
    say(e.h, beats(87.5, 2.0));
    send(e.h, osc_test::message("/dummy/region/clock"));
    renderFrames(e.h, 512);
    bool heard = false;
    for (const auto& r : heardBy(e.h)) {
        if (r.address != "/dummy/region/clock-is") continue;
        heard = true;
        CHECK(r.argInt(1) == 87500);
        CHECK(r.argInt(2) == 1);
    }
    CHECK(heard);
}

namespace {
// What the session's timeline reads at the first frame of the engine's latest block, and which frame that is.
struct GridAt { uint64_t frame; double beat; ClockworkClientClock clock; };
GridAt gridAtLatestBlock(ClockworkEmbed* h) {
    ClockworkClient* c = clockwork_embed_client(h);
    ClockworkClientSampleClock sc {};
    sc.struct_bytes = sizeof sc;
    REQUIRE(clockwork_client_sample_clock(c, &sc) == CLOCKWORK_OK);
    GridAt g {};
    g.clock.struct_bytes = sizeof g.clock;
    REQUIRE(clockwork_client_clock(c, &g.clock) == CLOCKWORK_OK);
    g.frame = sc.engine_frames;
    g.beat  = clockwork_client_beat_at(&g.clock, sc.dac_ntp);
    return g;
}
// What the clock told the handle's client since it was last asked: each tempo (notify/tempo) and each playing
// or stopped (notify/transport), in the order said.
struct ClockNotices { std::vector<double> tempos; std::vector<int32_t> playing; };
ClockNotices takeClockNotices(ClockworkEmbed* h) {
    ClockNotices heard;
    for (const auto& p : heardBy(h)) {
        if (p.address == "/clockwork/clock/notify/tempo") heard.tempos.push_back(p.argDouble(0));
        else if (p.address == "/clockwork/clock/notify/transport") heard.playing.push_back(p.argInt(0));
    }
    return heard;
}
// How far round a bar of `bar` beats `beat` is from `other`, by the shorter way.
double apartInBar(double beat, double other, double bar) {
    double d = std::fmod(beat - other, bar);
    if (d < 0.0) d += bar;
    return d > bar * 0.5 ? bar - d : d;
}
}

TEST_CASE("embed: the timeline follows what its host says, from the frame it was said at, its bars by request",
          "[embed][clock]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    std::vector<float> l(1024), r(1024);
    float* out[2] = { l.data(), r.data() };
    const double rate = lanes_test::kSampleRate;
    uint64_t served = 0;   // the host's own count of the frames it has been handed
    auto render = [&](uint32_t frames) {
        REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, frames) == frames);
        served += frames;
    };
    wantClock(e.h);
    // A buffer that is no multiple of the engine's block, so what is served and what is rendered differ.
    for (int i = 0; i < 8; ++i) render(100);
    takeClockNotices(e.h);

    // The host starts playing at 90 BPM in 3/4, a beat and a half into its bar.
    Said t;
    t.bpm = 90.0;
    t.num = 3;
    t.den = 4;
    t.playing = 1;
    t.placed = true;
    t.beat = 31.5;
    t.how = CLOCKWORK_BEAT_REQUEST;
    const uint64_t saidAt = served;
    say(e.h, t);
    for (int i = 0; i < 40; ++i) render(100);

    const auto hostBeatAt = [&](uint64_t frame, double from, uint64_t fromFrame, double bpm) {
        return from + static_cast<double>(frame - fromFrame) * bpm / (60.0 * rate);
    };
    // Half a millisecond of beats: what the bars may sit apart by (clock/host_transport.h).
    const auto tolerance = [](double bpm) { return 0.0005 * bpm / 60.0 + 1e-6; };
    {
        const GridAt g = gridAtLatestBlock(e.h);
        CHECK(g.clock.bpm == 90.0);
        CHECK(g.clock.meter_num == 3);
        CHECK(g.clock.meter_den == 4);
        CHECK(g.clock.is_playing == 1);
        REQUIRE(g.frame > saidAt);
        INFO("the grid's beat " << g.beat << " at frame " << g.frame << ", the host's " << hostBeatAt(g.frame, 31.5, saidAt, 90.0));
        CHECK(apartInBar(g.beat, hostBeatAt(g.frame, 31.5, saidAt, 90.0), 3.0) <= tolerance(90.0));
    }

    // The client was told, as it is of a Link peer's: the grid moved, and the host plays.
    {
        const ClockNotices heard = takeClockNotices(e.h);
        REQUIRE_FALSE(heard.tempos.empty());
        CHECK(heard.tempos.back() == 90.0);
        REQUIRE(heard.playing.size() == 1);
        CHECK(heard.playing[0] == 1);
    }

    // The host jumps to the middle of another bar and speeds up: the bars follow, and the beat count stays near.
    const double before = gridAtLatestBlock(e.h).beat;
    t.bpm = 120.0;
    t.beat = 302.0;   // bar 100, beat 2
    const uint64_t jumpedAt = served;
    say(e.h, t);
    for (int i = 0; i < 40; ++i) render(100);
    {
        const GridAt g = gridAtLatestBlock(e.h);
        CHECK(g.clock.bpm == 120.0);
        CHECK(apartInBar(g.beat, hostBeatAt(g.frame, 302.0, jumpedAt, 120.0), 3.0) <= tolerance(120.0));
        // 4000 frames at 120 BPM is under a beat; the move is under half a bar.
        CHECK(std::abs(g.beat - before) < 3.0);
    }

    // It stops: the tempo stands, the grid runs on, and its beat is not pulled back to where the host stopped.
    t.playing = 0;
    say(e.h, t);
    render(100);
    const GridAt stopped = gridAtLatestBlock(e.h);
    for (int i = 0; i < 40; ++i) {
        say(e.h, t);
        render(100);
    }
    {
        const GridAt g = gridAtLatestBlock(e.h);
        CHECK(g.clock.is_playing == 0);
        CHECK(g.clock.bpm == 120.0);
        CHECK(g.clock.beat_origin_ntp == stopped.clock.beat_origin_ntp);
        CHECK(g.beat > stopped.beat);
    }

    {
        const ClockNotices heard = takeClockNotices(e.h);
        REQUIRE_FALSE(heard.tempos.empty());
        CHECK(heard.tempos.back() == 120.0);
        REQUIRE(heard.playing.size() == 1);
        CHECK(heard.playing[0] == 0);
    }
}

TEST_CASE("embed: a host sweeping its tempo is told of to the clients a few times, the last tempo always",
          "[embed][clock]") {
    Handle e;
    REQUIRE(e.h != nullptr);
    std::vector<float> l(256), r(256);
    float* out[2] = { l.data(), r.data() };
    wantClock(e.h);
    for (int i = 0; i < 8; ++i) REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, 128) == 128);
    takeClockNotices(e.h);

    // A tempo that moves with every buffer for a fifth of a second of audio.
    double bpm = 0.0;
    const int buffers = static_cast<int>(0.2 * lanes_test::kSampleRate / 128.0);
    for (int i = 0; i < buffers; ++i) {
        bpm = 100.0 + 0.25 * i;
        REQUIRE(clockwork_embed_tempo(e.h, bpm, 0) == CLOCKWORK_OK);
        REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, 128) == 128);
    }
    // And then stands, for longer than the clients go untold.
    for (int i = 0; i < 16; ++i) REQUIRE(clockwork_embed_render(e.h, out, 2, nullptr, 0, 128) == 128);

    const ClockNotices heard = takeClockNotices(e.h);
    INFO(heard.tempos.size() << " notices for " << buffers << " changes of tempo");
    REQUIRE_FALSE(heard.tempos.empty());
    CHECK(heard.tempos.size() <= 13u);   // one in 20 ms of audio over 0.2 s, and the last
    CHECK(heard.tempos.size() < static_cast<size_t>(buffers));
    CHECK(heard.tempos.back() == bpm);
}
