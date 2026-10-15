// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_pumped_device.cpp — a device of the test's own, on the test's thread.
 *
 * A manual-pump engine renders blocks when told, which is what a test wants
 * for exact control; but a device is more than a block counter. It hands the
 * engine input and takes output, in buffers of whatever size the driver
 * chose, through ClockworkProcessor — the same worklet the device layer
 * drives. ClockworkEngine::pumpDevice is that edge with the test as the
 * driver: input in, output out, any size, with no thread or wall clock but
 * the caller's. These cases hold it to being a device.
 */
#include "EngineFixture.h"
#include "clockwork_client.h"
#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <cstdlib>
#include <random>
#include <sstream>
#include <vector>

extern "C" { extern uint8_t* shared_memory; }

using engine_test::Engine;
using engine_test::kSampleRate;

namespace {

constexpr int kChannels = 2;

ClockworkClientSampleClock sampleClock() {
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = clockwork_client_open_memory(shared_memory, ARENA_SIZE, &st);
    REQUIRE(st == CLOCKWORK_OK);
    ClockworkClientSampleClock sc{};
    sc.struct_bytes = sizeof(sc);
    REQUIRE(clockwork_client_sample_clock(c, &sc) == CLOCKWORK_OK);
    clockwork_client_close(c);
    return sc;
}

void echo(Engine& e) {
    const auto m = osc_test::message("/dummy/echo");
    REQUIRE(e.engine.ingest(m.ptr(), m.size(), 0) == CLOCKWORK_OK);
}

// What channel c carries at frame f: distinct per channel, never zero, and
// exact in a float, so a lost, doubled or misplaced frame is a wrong value.
float fed(int c, uint64_t f) {
    return static_cast<float>(c + 1) + static_cast<float>(f % 8191) / 8192.0f;
}

// One callback of `frames`: the stream from `next` in, what came out back.
std::vector<std::vector<float>> callback(Engine& e, uint64_t& next, int frames) {
    std::vector<std::vector<float>> in(kChannels, std::vector<float>(frames));
    std::vector<std::vector<float>> out(kChannels, std::vector<float>(frames, -1.0f));
    for (int c = 0; c < kChannels; ++c)
        for (int f = 0; f < frames; ++f) in[c][f] = fed(c, next + f);
    next += frames;
    const float* inPtrs[kChannels]  = { in[0].data(), in[1].data() };
    float*       outPtrs[kChannels] = { out[0].data(), out[1].data() };
    e.engine.pumpDevice(inPtrs, kChannels, outPtrs, kChannels, frames);
    return out;
}

// An engine whose device has inputs, and a buffer of `bufferFrames` (0: the
// first callback's size).
Engine withInputs(int bufferFrames = 0) {
    return Engine([bufferFrames](ClockworkEngine::Config& cfg) {
        cfg.numInputChannels = kChannels;
        cfg.bufferSize       = bufferFrames;
    });
}

} // namespace

// A device that stops and starts again begins its frames at 0, and a
// freewheel clock from an origin goes on from where the last frame ended:
// with no wall clock to re-anchor to, the frame count from the origin is the
// only time there is, and time going back would hold everything scheduled
// for as long again.
TEST_CASE("clock: a freewheel clock from an origin goes on through a restart, never back",
          "[clock][pumped]") {
    constexpr double kOrigin = 3976214400.0;
    Engine e([](ClockworkEngine::Config& cfg) { cfg.freewheelOriginNtp = kOrigin; });
    e.pump(0.1);
    const auto before = sampleClock();
    const double endedAt = before.dac_ntp + double(clockwork_block_size()) / kSampleRate;

    e.engine.restartManualPump();
    e.engine.pumpAudioBlock();
    const auto after = sampleClock();
    CHECK(after.engine_frames == 0);
    CHECK(after.dac_ntp == Catch::Approx(endedAt).margin(1e-6));
}

// Through the dummy's echo, output channel c is input channel c: what the
// device fed in comes back in the same callback, bit for bit, once the
// device's warmup (a real device's first callbacks are silence while its
// pages fault in) is over and the echo has been applied.
TEST_CASE("pumped device: what is fed in comes out of the echo, bit for bit",
          "[pumped]") {
    Engine e = withInputs();
    echo(e);
    const int block = static_cast<int>(clockwork_block_size());
    uint64_t next = 0;
    int matched = 0, firstMatch = -1;
    for (int i = 0; i < 64; ++i) {
        const uint64_t from = next;
        const auto out = callback(e, next, block);
        bool same = true;
        for (int c = 0; c < kChannels; ++c)
            for (int f = 0; f < block; ++f) same &= out[c][f] == fed(c, from + f);
        if (same) { ++matched; if (firstMatch < 0) firstMatch = i; }
        else      { INFO("callback " << i); CHECK(firstMatch < 0); }   // never after the first
    }
    INFO("first echoed callback " << firstMatch);
    CHECK(firstMatch >= 0);
    CHECK(firstMatch <= 6);   // the warmup, and the block that applies the echo
    CHECK(matched == 64 - firstMatch);
}

// A driver's buffer is whatever size it is, and need not be a multiple of
// the engine's block. One that keeps its size gives the stream back whole:
// its input is primed at the start with exactly the latency that size needs,
// rather than coming up short a few times and clicking. One whose size
// varies says so only by varying, so it may come up short now and then until
// it has fallen behind by all it ever can — less than a block, in all — and
// never again. Nothing is ever skipped or repeated. Varying sizes from a
// seed, which a failure names.
TEST_CASE("pumped device: callbacks of any size give back the stream, nothing lost or added",
          "[pumped]") {
    // A size of 0 here is a seeded random size each callback; anything else,
    // that size every time — 480 and 441 are Windows' 10 ms at 48 and 44.1 kHz.
    const int fixed = GENERATE(0, 64, 100, 441, 480, 512);
    const uint32_t seed = fixed ? 0u : GENERATE(1u, 2u, 3u, 0xC10C7u);
    constexpr int kLargest = 384;   // the random sizes' device buffer
    Engine e = withInputs(fixed ? fixed : kLargest);
    echo(e);
    const int block = static_cast<int>(clockwork_block_size());
    // Straight from the engine, whose output the standard fixes: a
    // std::uniform_int_distribution is each library's own, and gave each
    // platform different sizes for the same seed.
    std::mt19937 rng(seed);
    const auto size = [&rng] { return 1 + static_cast<int>(rng() % kLargest); };

    std::vector<float> heard;   // channel 0 out, callback after callback
    uint64_t next = 0;
    while (next < 48000) {
        const auto out = callback(e, next, fixed ? fixed : size());
        heard.insert(heard.end(), out[0].begin(), out[0].end());
    }
    // Where the stream comes through: the first frame that is frame k of it.
    size_t at = 0;
    while (at < heard.size() && heard[at] != fed(0, 0) && heard[at] < 1.0f) ++at;
    REQUIRE(at < heard.size());
    uint64_t k = 0;
    while (k < 8191 && heard[at] != fed(0, k)) ++k;
    INFO((fixed ? "callbacks of " + std::to_string(fixed) : "seed " + std::to_string(seed))
         << " (block " << block << "): the stream comes through at frame " << at << " as its frame " << k);

    // What came out, walked against what went in: each place the stream
    // breaks and how — silence put in, frames of it skipped, frames of it
    // given twice — the first dozen of them.
    std::ostringstream breaks;
    size_t silence = 0, skipped = 0, repeated = 0, lost = 0, told = 0;
    uint64_t expect = k;
    for (size_t i = at; i < heard.size();) {
        if (heard[i] == fed(0, expect)) { ++i; ++expect; continue; }
        if (heard[i] == 0.0f) {
            size_t run = 0;
            while (i < heard.size() && heard[i] == 0.0f) { ++i; ++run; }
            silence += run;
            if (told++ < 12) breaks << " " << run << " of silence at " << i - run << ";";
            continue;
        }
        // Where the stream picks up again: the frame of it this is, near
        // where it should have been (the signal repeats every 8191 frames).
        int64_t moved = 0;
        bool found = false;
        for (int64_t d = 1; d <= 4096 && !found; ++d)
            for (const int64_t s : { d, -d })
                if (!found && int64_t(expect) + s >= 0 && heard[i] == fed(0, uint64_t(int64_t(expect) + s))
                    && (i + 1 >= heard.size() || heard[i + 1] == fed(0, uint64_t(int64_t(expect) + s + 1)))) {
                    moved = s;
                    found = true;
                }
        if (!found) { ++lost; ++i; continue; }
        if (moved > 0) skipped += size_t(moved); else repeated += size_t(-moved);
        if (told++ < 12) breaks << " " << (moved > 0 ? "skipped " : "repeated ") << std::abs(moved)
                                << " at " << i << ";";
        expect = uint64_t(int64_t(expect) + moved);
    }
    INFO("breaks:" << (told ? breaks.str() : std::string(" none")) << (told > 12 ? " ..." : ""));
    INFO(silence << " frames of silence put in, " << skipped << " skipped, " << repeated
         << " repeated, " << lost << " not the stream at all");
    if (fixed) CHECK(silence == 0);
    else       CHECK(silence < size_t(block));
    CHECK(skipped == 0);
    CHECK(repeated == 0);
    CHECK(lost == 0);
}

// A device is the engine's one renderer. Once the test is the device, a
// block pumped beside it would be a second clock and a second sample count.
TEST_CASE("pumped device: an engine pumped as a device refuses a block pump", "[pumped]") {
    Engine e = withInputs();
    const int block = static_cast<int>(clockwork_block_size());
    uint64_t next = 0;
    for (int i = 0; i < 8; ++i) callback(e, next, block);
    const auto before = sampleClock();
    e.engine.pumpAudioBlock();
    CHECK(sampleClock().engine_frames == before.engine_frames);
}
