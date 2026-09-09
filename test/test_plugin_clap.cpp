// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_plugin_clap.cpp — the CLAP adapter, driven through src/plugin_host.h.
 *
 * Nothing here includes a CLAP header, which is the point: if a test needed
 * one, the abstraction would be leaking. The plugin under test is
 * plugins/clap/clockwork_clap_gain.c, built into the build tree and found by path
 * through CLOCKWORK_CLAP_TEST_PLUGIN — its behaviour is a spec (out = in * gain, one
 * parameter, 8-byte state) shared with the VST3 test plugin so the two can
 * later be diffed sample for sample.
 *
 * The cases drive clockwork_plugin_clap::* — the CLAP adapter's own entry points,
 * shapes identical to plugin_host.h's minus the prefix. That is deliberate:
 * plugin_host.h's C symbols belong to plugin_host.cpp, which tries each
 * compiled-in format in turn, so a failure to open through THEM says only
 * "no format claimed this file" and cannot say which one refused or why. The
 * last case in the file crosses that boundary once, to prove CLAP is reachable
 * through the public API and lands with the right format tag.
 *
 * Gains are powers of two throughout (0.5, 2.0, 0.25) so `in * gain` is exact
 * in binary floating point and the assertions can be equality rather than
 * tolerance. A tolerance would pass on an adapter that applied the gain one
 * frame late, which is exactly what the sample-accuracy case exists to catch.
 */
#include "DsoTestUtils.h"
#include "plugin_clap.h"
#include "rt_alloc.h"
#include "clock/clock_math.h"   // ntpToOscTimetag: a block time the clock case can place
#include "shared_memory.h"      // ClockworkClockState: the clock the transport case hands in

#if CLOCKWORK_CLAP_TEST_HOST_API
#  include "plugin_host.h"
#endif

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* kPluginPath = CLOCKWORK_CLAP_TEST_PLUGIN;

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kMaxBlock = 512;

// Two channels of ramp, one float per frame, values exact in binary floating
// point so every assertion below can be an equality.
struct Buffers {
    std::vector<float> in_l, in_r, out_l, out_r;
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;

    explicit Buffers(uint32_t frames) {
        in_l.resize(frames);
        in_r.resize(frames);
        out_l.assign(frames, -999.0f);
        out_r.assign(frames, -999.0f);
        for (uint32_t i = 0; i < frames; ++i) {
            in_l[i] = static_cast<float>(i + 1);
            in_r[i] = -static_cast<float>(i + 1);
        }
        in_ptrs  = { in_l.data(), in_r.data() };
        out_ptrs = { out_l.data(), out_r.data() };
    }
};

namespace clap_host = clockwork_plugin_clap;

struct Opened {
    clap_host::Instance* p = nullptr;
    ~Opened() { clap_host::close(p); }
};

double decode_le_double(const uint8_t* b) {
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(b[i]) << (8 * i);
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

std::filesystem::path write_junk_file() {
    static int seq = 0;
    auto path = std::filesystem::temp_directory_path() /
                ("clockwork-not-a-plugin-" + std::to_string(++seq) + ".bin");
    std::ofstream f(path, std::ios::binary);
    f << "this is not a shared object, let alone a CLAP one\n";
    f.close();
    return path;
}

} // namespace

TEST_CASE("clap: scanning finds the in-tree test plugin", "[plugin][clap]") {
    PluginDesc descs[4]{};
    const uint32_t n = clap_host::scan(kPluginPath, descs, 4);

    REQUIRE(n == 1);
    CHECK(descs[0].format == kPluginFormatClap);
    CHECK(descs[0].index == 0);
    REQUIRE(descs[0].id != nullptr);
    REQUIRE(descs[0].name != nullptr);
    REQUIRE(descs[0].vendor != nullptr);
    CHECK(std::string(descs[0].id)     == "clockwork.clap.gain");
    CHECK(std::string(descs[0].name)   == "Clockwork Test Gain");
    CHECK(std::string(descs[0].vendor) == "Clockwork");
}

TEST_CASE("clap: scan reports the count even with no room to write it", "[plugin][clap]") {
    // The header says the return is how many EXIST, not how many were written.
    CHECK(clap_host::scan(kPluginPath, nullptr, 0) == 1);

    PluginDesc one{};
    CHECK(clap_host::scan(kPluginPath, &one, 0) == 1);
}

TEST_CASE("clap: scanning something that is not a plugin refuses rather than fails",
          "[plugin][clap]") {
    const auto junk = write_junk_file();
    CHECK(clap_host::scan(junk.string().c_str(), nullptr, 0) == 0);

    PluginDesc descs[2]{};
    CHECK(clap_host::scan(junk.string().c_str(), descs, 2) == 0);
    std::filesystem::remove(junk);

    // A directory, a path that is not there at all, and no path at all.
    CHECK(clap_host::scan(std::filesystem::temp_directory_path().string().c_str(), descs, 2) == 0);
    CHECK(clap_host::scan("/no/such/file.clap", descs, 2) == 0);
    CHECK(clap_host::scan("", descs, 2) == 0);
}

TEST_CASE("clap: opening and closing", "[plugin][clap]") {
    const char* err = "unset";
    Opened o;
    o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);

    REQUIRE(o.p != nullptr);
    CHECK(err == nullptr);
    CHECK(clap_host::latency(o.p) == 0u);
}

TEST_CASE("clap: opening a path that is not there fails with a message", "[plugin][clap]") {
    const char* err = nullptr;
    clap_host::Instance* p = clap_host::open("/no/such/plugin.clap", 0, kSampleRate, kMaxBlock, &err);
    CHECK(p == nullptr);
    REQUIRE(err != nullptr);
    CHECK(std::strlen(err) > 0);
    CHECK(std::string(err).find("/no/such/plugin.clap") != std::string::npos);

    // Same for a file that exists but is not a plugin, and for an index past
    // the end of a file that is.
    const auto junk = write_junk_file();
    err = nullptr;
    CHECK(clap_host::open(junk.string().c_str(), 0, kSampleRate, kMaxBlock, &err) == nullptr);
    CHECK(err != nullptr);
    std::filesystem::remove(junk);

    err = nullptr;
    CHECK(clap_host::open(kPluginPath, 7, kSampleRate, kMaxBlock, &err) == nullptr);
    REQUIRE(err != nullptr);
    CHECK(std::string(err).find("index") != std::string::npos);
}

TEST_CASE("clap: the parameter is enumerated as the plugin declares it", "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    REQUIRE(clap_host::param_count(o.p) == 1u);

    PluginParam pp{};
    REQUIRE(clap_host::param_info(o.p, 0, &pp) == 1);
    CHECK(pp.id == 0u);
    REQUIRE(pp.name != nullptr);
    CHECK(std::string(pp.name) == "gain");
    CHECK(pp.min == 0.0);
    CHECK(pp.max == 2.0);
    CHECK(pp.value == 0.5);

    PluginParam none{};
    CHECK(clap_host::param_info(o.p, 1, &none) == 0);
}

TEST_CASE("clap: processing applies the default gain exactly", "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 128;
    Buffers b(kFrames);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);

    for (uint32_t i = 0; i < kFrames; ++i) {
        REQUIRE(b.out_l[i] == b.in_l[i] * 0.5f);
        REQUIRE(b.out_r[i] == b.in_r[i] * 0.5f);
    }
}

TEST_CASE("clap: setting the parameter changes the output", "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    clap_host::param_set(o.p, 0, 2.0, 0);

    Buffers b(kFrames);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i)
        REQUIRE(b.out_l[i] == b.in_l[i] * 2.0f);

    // And it persists into the next block without being set again.
    Buffers b2(kFrames);
    clap_host::process(o.p, b2.in_ptrs.data(), 2, b2.out_ptrs.data(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i)
        REQUIRE(b2.out_l[i] == b2.in_l[i] * 2.0f);

    // The plugin has really taken it, not just the adapter's cache.
    PluginParam pp{};
    REQUIRE(clap_host::param_info(o.p, 0, &pp) == 1);
    CHECK(pp.value == 2.0);
}

// The valuable one. An adapter that queues the event but drops its frame
// offset, or applies it a frame early or late, fails here and nowhere else.
TEST_CASE("clap: a parameter change lands on its exact frame", "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 128;
    constexpr uint32_t kAt     = 64;

    clap_host::param_set(o.p, 0, 2.0, kAt);

    Buffers b(kFrames);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);

    for (uint32_t i = 0; i < kAt; ++i) {
        REQUIRE(b.out_l[i] == b.in_l[i] * 0.5f);
        REQUIRE(b.out_r[i] == b.in_r[i] * 0.5f);
    }
    for (uint32_t i = kAt; i < kFrames; ++i) {
        REQUIRE(b.out_l[i] == b.in_l[i] * 2.0f);
        REQUIRE(b.out_r[i] == b.in_r[i] * 2.0f);
    }
    // Stated once more at the boundary itself, so a failure reads as an
    // off-by-one rather than as "the whole block is wrong".
    CHECK(b.out_l[kAt - 1] == b.in_l[kAt - 1] * 0.5f);
    CHECK(b.out_l[kAt]     == b.in_l[kAt]     * 2.0f);
}

TEST_CASE("clap: several changes in one block each land on their own frame",
          "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 96;
    // Queued out of order on purpose: the adapter must sort by frame, and the
    // event list CLAP is handed must be ordered.
    clap_host::param_set(o.p, 0, 0.25, 64);
    clap_host::param_set(o.p, 0, 2.0,  32);

    Buffers b(kFrames);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);

    for (uint32_t i = 0; i < 32; ++i)  REQUIRE(b.out_l[i] == b.in_l[i] * 0.5f);
    for (uint32_t i = 32; i < 64; ++i) REQUIRE(b.out_l[i] == b.in_l[i] * 2.0f);
    for (uint32_t i = 64; i < kFrames; ++i) REQUIRE(b.out_l[i] == b.in_l[i] * 0.25f);
}

TEST_CASE("clap: outputs the plugin has no input for are silent", "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 32;
    Buffers b(kFrames);
    // One input channel supplied, two asked for on the way out.
    clap_host::process(o.p, b.in_ptrs.data(), 1, b.out_ptrs.data(), 2, kFrames, 0);

    for (uint32_t i = 0; i < kFrames; ++i) {
        REQUIRE(b.out_l[i] == b.in_l[i] * 0.5f);
        REQUIRE(b.out_r[i] == 0.0f);
    }
}

TEST_CASE("clap: state saves, and restores a gain that has since changed",
          "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 32;
    Buffers warm(kFrames);

    // Take the gain to 2.0 and let a block carry the event into the plugin.
    clap_host::param_set(o.p, 0, 2.0, 0);
    clap_host::process(o.p, warm.in_ptrs.data(), 2, warm.out_ptrs.data(), 2, kFrames, 0);

    // Size query first: the header allows out == NULL to mean "how big?".
    const uint32_t need = clap_host::state_save(o.p, nullptr, 0);
    REQUIRE(need == 8u);

    std::vector<uint8_t> blob(need);
    REQUIRE(clap_host::state_save(o.p, blob.data(), need) == need);
    CHECK(decode_le_double(blob.data()) == 2.0);

    // Move it somewhere else and prove it moved.
    clap_host::param_set(o.p, 0, 0.25, 0);
    Buffers moved(kFrames);
    clap_host::process(o.p, moved.in_ptrs.data(), 2, moved.out_ptrs.data(), 2, kFrames, 0);
    REQUIRE(moved.out_l[kFrames - 1] == moved.in_l[kFrames - 1] * 0.25f);

    // Reload and prove it came back, in the parameter and in the audio.
    REQUIRE(clap_host::state_load(o.p, blob.data(), need) == 1);

    PluginParam pp{};
    REQUIRE(clap_host::param_info(o.p, 0, &pp) == 1);
    CHECK(pp.value == 2.0);

    Buffers back(kFrames);
    clap_host::process(o.p, back.in_ptrs.data(), 2, back.out_ptrs.data(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i)
        REQUIRE(back.out_l[i] == back.in_l[i] * 2.0f);
}

TEST_CASE("clap: a blob the plugin does not recognise is refused", "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    const uint8_t truncated[3] = { 1, 2, 3 };
    CHECK(clap_host::state_load(o.p, truncated, 3) == 0);

    // Eight bytes, but decoding to a gain outside the declared range.
    const double wild = 1e9;
    uint64_t bits; std::memcpy(&bits, &wild, sizeof(bits));
    uint8_t out_of_range[8];
    for (int i = 0; i < 8; ++i) out_of_range[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFFu);
    CHECK(clap_host::state_load(o.p, out_of_range, 8) == 0);

    // Refusing left the gain where it was.
    PluginParam pp{};
    REQUIRE(clap_host::param_info(o.p, 0, &pp) == 1);
    CHECK(pp.value == 0.5);
}

TEST_CASE("clap: the session clock arrives as the block's transport", "[plugin][clap][clock]") {
    Opened o;
    const char* err = nullptr;
    o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    // The plugin keeps the last transport it was handed, and hands it back in
    // plain numbers so this file still includes no CLAP header.
    void* h = dlopen(kPluginPath, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != nullptr);
    auto last = reinterpret_cast<bool (*)(double*, double*, double*, int*, int*, int*, int*)>(
        dlsym(h, "clockwork_clap_gain_last_transport"));
    REQUIRE(last != nullptr);

    constexpr uint32_t kFrames = 16;
    Buffers b(kFrames);
    double tempo = 0, beat = 0, barStart = 0;
    int bar = 0, num = 0, den = 0, playing = 0;

    // No clock: free running, which CLAP spells as no transport at all.
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);
    CHECK_FALSE(last(&tempo, &beat, &barStart, &bar, &num, &den, &playing));

    // 120 BPM from beat 0 at NTP 1000, in 7/8, playing. The block at 1010 is
    // beat 20: bar 5 of 3.5 quarter notes, which began at beat 17.5.
    ClockworkClockState clock;
    ClockworkClockState::initDefaults(clock);
    clock.setTempo(120.0, 1000.0);
    clock.setMeter(7, 8);
    clock.setTransport(true, 1000.0);
    clap_host::set_clock(o.p, &clock);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames,
                       clockwork::ntpToOscTimetag(1010.0));
    REQUIRE(last(&tempo, &beat, &barStart, &bar, &num, &den, &playing));
    CHECK(tempo == 120.0);
    CHECK(beat == 20.0);         // exact in CLAP's 32-bit fixed point
    CHECK(barStart == 17.5);
    CHECK(bar == 5);
    CHECK(num == 7);
    CHECK(den == 8);
    CHECK(playing == 1);
    // The gain still applied: a transport is information, not a gate.
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(b.out_l[i] == b.in_l[i] * 0.5f);

    clock.setTransport(false, 1011.0);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames,
                       clockwork::ntpToOscTimetag(1011.0));
    REQUIRE(last(&tempo, &beat, &barStart, &bar, &num, &den, &playing));
    CHECK(playing == 0);
    CHECK(beat == 22.0);
    CHECK(bar == 6);
    CHECK(barStart == 21.0);

    clap_host::set_clock(o.p, nullptr);
    clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);
    CHECK_FALSE(last(&tempo, &beat, &barStart, &bar, &num, &den, &playing));
    dlclose(h);
}

TEST_CASE("clap: the rt-alloc guard is suspended across the plugin and restored after",
          "[plugin][clap]") {
    const char* err = nullptr;
    Opened o; o.p = clap_host::open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    Buffers b(kFrames);

    // plugin_process is called from inside clockwork's audio-thread guard.
    // Whatever the plugin does in there, the guard must be back on afterwards
    // — the hole is one call wide, not thread wide.
    {
        rt_alloc::Guard guard;
        REQUIRE(rt_alloc::g_in_rt == true);
        clap_host::process(o.p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);
        CHECK(rt_alloc::g_in_rt == true);
    }
    CHECK(rt_alloc::g_in_rt == false);

    for (uint32_t i = 0; i < kFrames; ++i)
        REQUIRE(b.out_l[i] == b.in_l[i] * 0.5f);
}

#if CLOCKWORK_CLAP_TEST_HOST_API
// One crossing of the public boundary: plugin_host.cpp dispatches to whichever
// format claims the file, so this says the CLAP adapter is actually reachable
// through plugin_host.h and is tagged as CLAP when it answers.
TEST_CASE("clap: reachable through plugin_host.h with the right format tag",
          "[plugin][clap]") {
    PluginDesc desc{};
    REQUIRE(plugin_scan(kPluginPath, &desc, 1) == 1);
    CHECK(desc.format == kPluginFormatClap);

    const char* err = "unset";
    HostedPlugin* p = plugin_open(kPluginPath, 0, kSampleRate, kMaxBlock, &err);
    REQUIRE(p != nullptr);
    CHECK(plugin_format(p) == kPluginFormatClap);
    CHECK(plugin_latency(p) == 0u);
    REQUIRE(plugin_param_count(p) == 1u);

    constexpr uint32_t kFrames = 64;
    constexpr uint32_t kAt     = 16;
    Buffers b(kFrames);
    plugin_param_set(p, 0, 2.0, kAt);
    plugin_process(p, b.in_ptrs.data(), 2, b.out_ptrs.data(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kAt; ++i)       REQUIRE(b.out_l[i] == b.in_l[i] * 0.5f);
    for (uint32_t i = kAt; i < kFrames; ++i) REQUIRE(b.out_l[i] == b.in_l[i] * 2.0f);

    plugin_close(p);
}
#endif
