// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_plugin_parity.cpp — the test the two test plugins exist for.
 *
 * `plugin_host.h` claims one abstraction hides two plugin formats. Each
 * format's own suite proves its adapter works; neither can prove the claim,
 * because a suite written against one format will happily agree with an
 * adapter that leaks that format's conventions.
 *
 * So: the CLAP and VST3 test plugins implement deliberately IDENTICAL
 * behaviour — out = in * gain, one parameter, id 0, range 0..2, default 0.5,
 * state as an 8-byte little-endian double — and everything here drives both
 * through the public C API and asserts the results agree. Where they cannot
 * agree, the abstraction has leaked.
 *
 * The parameter is the sharp end. VST3 insists parameters cross its wire
 * normalised to 0..1, CLAP uses real ranges, and `plugin_param_set(id, 1.5)`
 * must mean gain 1.5 through both. An adapter that forgot to denormalise
 * would pass its own suite — 0.25 in, 0.25 out, self-consistent — and fail
 * here against a format that never normalised anything.
 */
#include "DsoTestUtils.h"
#include "plugin_host.h"
#include "clock/clock_math.h"   // ntpToOscTimetag: a block time both formats are placed at
#include "shared_memory.h"      // ClockworkClockState: the one clock both are handed

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using Catch::Approx;

#if defined(CLOCKWORK_CLAP_TEST_PLUGIN) && defined(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE)

namespace {

constexpr double   kRate   = 48000.0;
constexpr uint32_t kBlock  = 128;
constexpr uint32_t kGainId = 0;

struct Opened {
    struct HostedPlugin* p = nullptr;
    explicit Opened(const char* path) {
        const char* err = nullptr;
        p = plugin_open(path, 0, kRate, kBlock, &err);
        INFO("opening " << path << ": " << (err ? err : "(no error reported)"));
        REQUIRE(p != nullptr);
    }
    ~Opened() { plugin_close(p); }
};

// A signal with no symmetry a bug could hide in: every frame distinct, both
// signs present, and the two channels different from each other.
std::vector<std::vector<float>> rampIn(uint32_t channels, uint32_t frames) {
    std::vector<std::vector<float>> v(channels, std::vector<float>(frames, 0.0f));
    for (uint32_t c = 0; c < channels; ++c)
        for (uint32_t i = 0; i < frames; ++i)
            v[c][i] = (static_cast<float>(i) / static_cast<float>(frames) - 0.5f)
                    * (c == 0 ? 1.0f : -0.75f);
    return v;
}

// Run one block at `block_time` and return the output, channel-major.
std::vector<std::vector<float>> runBlockAt(struct HostedPlugin* p,
                                           const std::vector<std::vector<float>>& in,
                                           uint32_t frames, int64_t block_time) {
    const uint32_t ch = static_cast<uint32_t>(in.size());
    std::vector<std::vector<float>> out(ch, std::vector<float>(frames, -99.0f));
    std::vector<const float*> inp(ch);
    std::vector<float*>       outp(ch);
    for (uint32_t c = 0; c < ch; ++c) { inp[c] = in[c].data(); outp[c] = out[c].data(); }
    plugin_process(p, inp.data(), ch, outp.data(), ch, frames, block_time);
    return out;
}

std::vector<std::vector<float>> runBlock(struct HostedPlugin* p,
                                         const std::vector<std::vector<float>>& in,
                                         uint32_t frames) {
    return runBlockAt(p, in, frames, 0);
}

} // namespace

TEST_CASE("plugin parity: both formats load through one abstraction", "[plugin][parity]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);

    // The one thing that SHOULD differ.
    CHECK(plugin_format(clap.p) == kPluginFormatClap);
    CHECK(plugin_format(vst3.p) == kPluginFormatVst3);

    // Everything else must not.
    CHECK(plugin_param_count(clap.p) == plugin_param_count(vst3.p));
    CHECK(plugin_latency(clap.p) == plugin_latency(vst3.p));
}

TEST_CASE("plugin parity: the parameter reads the same through both", "[plugin][parity]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);

    PluginParam a{}, b{};
    REQUIRE(plugin_param_info(clap.p, 0, &a));
    REQUIRE(plugin_param_info(vst3.p, 0, &b));

    // In PLAIN units, both. VST3 carries this parameter normalised to 0..1 on
    // its own wire; if that leaked, max would read 1.0 here and the value
    // 0.25 rather than 0.5.
    CHECK(a.min == Approx(b.min));
    CHECK(a.max == Approx(b.max));
    CHECK(a.value == Approx(b.value));
    CHECK(a.max == Approx(2.0));
    CHECK(a.value == Approx(0.5));
}

TEST_CASE("plugin parity: identical audio, sample for sample", "[plugin][parity]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);

    const auto in = rampIn(2, kBlock);

    SECTION("at the default gain") {
        const auto c = runBlock(clap.p, in, kBlock);
        const auto v = runBlock(vst3.p, in, kBlock);
        for (uint32_t ch = 0; ch < 2; ++ch)
            for (uint32_t i = 0; i < kBlock; ++i) {
                INFO("channel " << ch << " frame " << i);
                REQUIRE(c[ch][i] == Approx(v[ch][i]).margin(1e-6));
                REQUIRE(c[ch][i] == Approx(in[ch][i] * 0.5f).margin(1e-6));
            }
    }

    SECTION("after the same parameter change") {
        // 1.5 is deliberately not representable as a round number in VST3's
        // 0..1 space (it is 0.75 normalised), so a missing conversion shows.
        plugin_param_set(clap.p, kGainId, 1.5, 0);
        plugin_param_set(vst3.p, kGainId, 1.5, 0);
        const auto c = runBlock(clap.p, in, kBlock);
        const auto v = runBlock(vst3.p, in, kBlock);
        for (uint32_t ch = 0; ch < 2; ++ch)
            for (uint32_t i = 0; i < kBlock; ++i) {
                REQUIRE(c[ch][i] == Approx(v[ch][i]).margin(1e-6));
                REQUIRE(c[ch][i] == Approx(in[ch][i] * 1.5f).margin(1e-6));
            }
    }
}

TEST_CASE("plugin parity: a change lands on the same frame in both", "[plugin][parity]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);

    const auto in = rampIn(1, kBlock);
    constexpr uint32_t kAt = 64;

    plugin_param_set(clap.p, kGainId, 2.0, kAt);
    plugin_param_set(vst3.p, kGainId, 2.0, kAt);
    const auto c = runBlock(clap.p, in, kBlock);
    const auto v = runBlock(vst3.p, in, kBlock);

    for (uint32_t i = 0; i < kBlock; ++i) {
        const float expected = in[0][i] * (i < kAt ? 0.5f : 2.0f);
        INFO("frame " << i << (i < kAt ? " (before the change)" : " (after)"));
        REQUIRE(c[0][i] == Approx(expected).margin(1e-6));
        REQUIRE(v[0][i] == Approx(expected).margin(1e-6));
        REQUIRE(c[0][i] == Approx(v[0][i]).margin(1e-6));
    }
}

TEST_CASE("plugin parity: state is interchangeable between formats", "[plugin][parity]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);

    plugin_param_set(clap.p, kGainId, 1.25, 0);
    (void)runBlock(clap.p, rampIn(1, kBlock), kBlock);   // let it take effect

    uint8_t blob[64];
    const uint32_t n = plugin_state_save(clap.p, blob, sizeof blob);
    REQUIRE(n == 8);   // both spell it as one little-endian double

    // The blobs are the same shape by construction, so VST3 can eat CLAP's.
    // Nothing in plugin_host.h promises this — a real plugin's state is its
    // own business — but here it pins that both test plugins really do
    // implement the one agreed behaviour rather than merely looking alike.
    REQUIRE(plugin_state_load(vst3.p, blob, n));

    const auto in = rampIn(1, kBlock);
    const auto v = runBlock(vst3.p, in, kBlock);
    for (uint32_t i = 0; i < kBlock; ++i)
        REQUIRE(v[0][i] == Approx(in[0][i] * 1.25f).margin(1e-6));
}



// ── Health: which plugin is misbehaving, and does the log say so ─────────────
//
// A plugin runs third-party code on the audio thread. Clockwork cannot make
// it well behaved; it can only notice and attribute. These pin that it does
// both — and that a user submitting a debug log hands over the culprit's name
// without having to know there was anything to look for.

TEST_CASE("plugin health: every call is timed and attributed", "[plugin][parity][health]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    const auto in = rampIn(2, kBlock);

    PluginHealth h{};
    REQUIRE(plugin_health(clap.p, &h));
    CHECK(h.calls == 0);

    for (int i = 0; i < 8; ++i) (void)runBlock(clap.p, in, kBlock);

    REQUIRE(plugin_health(clap.p, &h));
    CHECK(h.calls == 8);
    CHECK(h.total_ns > 0);
    CHECK(h.max_ns > 0);
    CHECK(h.max_ns <= h.total_ns);

    // The budget is the block's own duration: 128 frames at 48 kHz is 2.67 ms.
    // A gain plugin must beat that by orders of magnitude, so this doubles as a
    // sanity check that we are timing the call and not the whole test.
    CHECK(h.budget_ns == Approx(kBlock / kRate * 1e9).epsilon(0.01));
    CHECK(h.budget_overruns == 0);
    CHECK(h.max_ns < h.budget_ns);

    // Named, always — an unattributed complaint is what this exists to prevent.
    REQUIRE(h.name != nullptr);
    CHECK(std::string(h.name).size() > 0);
}

TEST_CASE("plugin health: the log line names the plugin", "[plugin][parity][health]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);
    const auto in = rampIn(2, kBlock);
    for (int i = 0; i < 4; ++i) { (void)runBlock(clap.p, in, kBlock);
                                  (void)runBlock(vst3.p, in, kBlock); }

    char line[256];
    const uint32_t n = plugin_health_line(clap.p, line, sizeof line);
    REQUIRE(n > 0);
    const std::string s(line, n);
    INFO(s);
    CHECK(s.find("plugin ") == 0);
    CHECK(s.find("4 calls") != std::string::npos);
    CHECK(s.find("within budget") != std::string::npos);

    // Both formats produce a line, and they are distinguishable — otherwise a
    // log with two plugins in it tells you nothing about which one to blame.
    char other[256];
    REQUIRE(plugin_health_line(vst3.p, other, sizeof other) > 0);
    CHECK(std::string(other) != s);
}

TEST_CASE("plugin health: a small buffer truncates rather than overruns",
          "[plugin][parity][health]") {
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    // The guard byte sits PAST the region handed over: snprintf legitimately
    // writes its terminator at the last byte of the buffer it is given, so a
    // sentinel inside the buffer proves nothing.
    char buf[16];
    std::memset(buf, 0x7f, sizeof buf);
    constexpr uint32_t kGiven = 8;
    const uint32_t n = plugin_health_line(clap.p, buf, kGiven);
    CHECK(n < kGiven);
    CHECK(buf[kGiven] == 0x7f);          // nothing written past what we gave
    CHECK(std::strlen(buf) < kGiven);    // and it is still a C string
}

TEST_CASE("plugin parity: both formats are told the same musical time", "[plugin][parity][clock]") {
    // One clock, one block time, two formats: the beat, the bar and the meter
    // a VST3 sees in its ProcessContext must be the ones a CLAP sees in its
    // transport, or a synced delay on one track would drift from the other.
    Opened clap(CLOCKWORK_CLAP_TEST_PLUGIN);
    Opened vst3(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE);

    void* hc = dlopen(CLOCKWORK_CLAP_TEST_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    void* hv = dlopen(CLOCKWORK_VST3_TEST_PLUGIN_SO, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(hc != nullptr);
    REQUIRE(hv != nullptr);
    auto lastClap = reinterpret_cast<bool (*)(double*, double*, double*, int*, int*, int*, int*)>(
        dlsym(hc, "clockwork_clap_gain_last_transport"));
    auto lastVst3 = reinterpret_cast<bool (*)(double*, double*, double*, int*, int*, unsigned*)>(
        dlsym(hv, "ClockworkTestGainLastContext"));
    REQUIRE(lastClap != nullptr);
    REQUIRE(lastVst3 != nullptr);

    ClockworkClockState clock;
    ClockworkClockState::initDefaults(clock);
    clock.setTempo(93.0, 2000.0);
    clock.setMeter(5, 4);
    clock.setTransport(true, 2000.0);
    plugin_set_clock(clap.p, &clock);
    plugin_set_clock(vst3.p, &clock);

    const auto in = rampIn(2, kBlock);
    const int64_t at = clockwork::ntpToOscTimetag(2017.25);
    runBlockAt(clap.p, in, kBlock, at);
    runBlockAt(vst3.p, in, kBlock, at);

    double ct = 0, cb = 0, cbar = 0; int cnum = 0, cden = 0, cbarn = 0, cplay = 0;
    double vt = 0, vb = 0, vbar = 0; int vnum = 0, vden = 0; unsigned vstate = 0;
    REQUIRE(lastClap(&ct, &cb, &cbar, &cbarn, &cnum, &cden, &cplay));
    REQUIRE(lastVst3(&vt, &vb, &vbar, &vnum, &vden, &vstate));
    CHECK(ct == vt);
    CHECK(cb == Approx(vb).margin(1e-9));      // CLAP's 2^-31 beat grain
    CHECK(cbar == Approx(vbar).margin(1e-9));
    CHECK(cnum == vnum);
    CHECK(cden == vden);
    CHECK(cnum == 5);
    // 17.25 s at 93 BPM is beat 26.7375: bar 5 of 5 quarter notes, from 25.
    CHECK(vb == Approx(26.7375));
    CHECK(vbar == Approx(25.0));
    CHECK(cbarn == 5);
    dlclose(hc);
    dlclose(hv);
}

#endif // both test plugins present
