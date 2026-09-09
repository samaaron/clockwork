// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_audio_path.cpp — does clockwork actually carry samples, on time?
 *
 * This is the one test the suite cannot do without. Everything else here
 * checks a mechanism in isolation; this one drives whole blocks through the
 * real path — clockwork_ingress_write -> clockwork_tick -> clockwork's classifier and
 * timed queue -> dsp_process, which writes straight into clockwork's own
 * staging buffer -> clockwork_audio_out, which is what a host hands its device — and
 * asserts that the samples arriving there are exactly the ones the DSP wrote,
 * at exactly the frames it wrote them.
 *
 * It asserts EXACT values, not "not silent". The placeholder DSP (dsp/dummy)
 * emits a rectangular pulse train whose every sample is a pure function of its
 * absolute index in the stream:
 *
 *     period = round(period_ms * rate / 1000)      default 500 ms -> 24000 @48k
 *     width  = round(width_ms  * rate / 1000)      default  10 ms ->   480 @48k
 *     on when (n % period) < width, off otherwise
 *     left  (even channels): +0.2f on, 0.0f off
 *     right (odd  channels): -0.2f on, 0.0f off   — the left, inverted
 *
 * so this file can recompute any sample from its absolute frame index alone.
 * That is the whole reason the placeholder emits signal rather than silence: a
 * test that asserts zeros passes just as happily when the audio path is not
 * wired up at all, and would have proved nothing.
 *
 * A pulse rather than a tone, because this is a TIMING instrument. Every edge
 * is a function of the absolute frame index, so drift shows up as an edge in
 * the wrong place, a dropped block as a missing pulse, and a duplicated block
 * as a doubled one. None of that is visible in a continuous tone, which is why
 * the edge cases below scan for transitions and check the frame each one
 * lands on rather than merely comparing sample values.
 *
 * The right channel is the left's polarity inversion: same waveform, same
 * instant, opposite sign. That makes `left[i] + right[i] == 0` an EXACT
 * invariant at every sample, and an exact invariant catches what per-channel
 * comparison cannot — one sample of inter-channel skew spikes the sum to
 * +/-2*amp at every edge while each channel on its own still looks perfectly
 * plausible, a duplicated channel sums to 2*left rather than 0, and a swap or
 * a lost sign shows up in which channel is positive.
 *
 * The expected values are kept in step with dsp/dummy/dummy_dsp.cpp BY HAND,
 * deliberately: if the placeholder's pattern changes, this test must fail
 * rather than silently follow it. Sharing a header would make the assertion
 * tautological.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

constexpr float    kOn  = 0.2f;   // left; the right channel is its negative
constexpr float    kOff = 0.0f;

// The placeholder's defaults, and the frame counts they become at the
// fixture's rate. Rounding is to nearest, once, at dsp_new — everything
// downstream counts frames, so the train can never drift against the audio
// clock however long it runs.
constexpr uint32_t kDefaultWidthMs  = 10;
constexpr uint32_t kDefaultPeriodMs = 500;

uint64_t framesFor(uint32_t ms) {
    return static_cast<uint64_t>(ms * lanes_test::kSampleRate / 1000.0 + 0.5);
}

// Odd channels are the even ones inverted; the pulse grid itself is shared.
float channelSign(uint32_t channel) { return (channel % 2) ? -1.0f : 1.0f; }

float expectedSample(uint32_t channel, uint64_t frameIndex,
                     uint64_t width, uint64_t period) {
    return ((frameIndex % period) < width) ? channelSign(channel) * kOn : kOff;
}

// One channel's samples across `blocks` consecutive ticks, plus the absolute
// frame index of the first of them. Everything below asserts against absolute
// indices, so it does not matter what the rest of the suite left the shared
// fixture's stream position at.
struct Capture {
    uint64_t                        firstFrame = 0;
    uint32_t                        blockSize  = 0;
    std::vector<std::vector<float>> channel;   // [channel][sample]
};

Capture capture(int blocks) {
    const uint32_t bl = lanes_test::boot();
    Capture cap;
    cap.blockSize = bl;
    cap.channel.resize(lanes_test::kOutChannels);
    for (int b = 0; b < blocks; ++b) {
        const uint64_t at = lanes_test::tick();
        if (b == 0) cap.firstFrame = at;
        const float* out = clockwork_audio_out();
        REQUIRE(out != nullptr);
        for (uint32_t ch = 0; ch < lanes_test::kOutChannels; ++ch)
            cap.channel[ch].insert(cap.channel[ch].end(),
                                   out + ch * bl, out + (ch + 1) * bl);
    }
    return cap;
}

// Every transition in a captured channel, as the absolute frame index of the
// FIRST sample of the new level. An edge lands ON that frame: frame k*period
// is already at full amplitude. `onset` is the start of a pulse rather than
// "the sample went up", so it reads the same on the inverted channel.
struct Edge { uint64_t frame; bool onset; };

std::vector<Edge> edgesOf(const Capture& cap, uint32_t ch) {
    std::vector<Edge> edges;
    const auto& v = cap.channel[ch];
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] != v[i - 1])
            edges.push_back(Edge{ cap.firstFrame + i, v[i] != kOff });
    }
    return edges;
}

// Reconfigure the placeholder over OSC and let the tick that applies it pass.
// The verb is /dummy/pulse ,ii <width_ms> <period_ms> — the DSP's own word,
// not clockwork's, so it takes the default route and reaches dsp_osc.
void setPulse(uint32_t widthMs, uint32_t periodMs) {
    const auto msg = osc_test::message("/dummy/pulse",
                                       static_cast<int32_t>(widthMs),
                                       static_cast<int32_t>(periodMs));
    REQUIRE(lanes_test::ingress(msg.ptr(), msg.size(), /*sourceId=*/0));
    lanes_test::tick();          // the block that drains and applies it
    lanes_test::drainRt();       // keep the egress ring tidy for other cases
}

} // namespace

TEST_CASE("audio path: a rendered block reaches the hardware join intact", "[audio][lanes]") {
    const uint32_t bl = lanes_test::boot();
    REQUIRE(bl > 0);

    const uint64_t width  = framesFor(kDefaultWidthMs);
    const uint64_t period = framesFor(kDefaultPeriodMs);

    const uint64_t at  = lanes_test::tick();
    const float*   out = clockwork_audio_out();
    REQUIRE(out != nullptr);

    // Channel-major: channel c occupies [c*bl, (c+1)*bl). Exact equality, not
    // a tolerance: the DSP writes 0.2f or 0.0f directly into this buffer and
    // clockwork applies no arithmetic on the way to the join, so anything
    // but an exact match means the samples took a detour.
    for (uint32_t ch = 0; ch < lanes_test::kOutChannels; ++ch) {
        for (uint32_t i = 0; i < bl; ++i) {
            const float got  = out[ch * bl + i];
            const float want = expectedSample(ch, at + i, width, period);
            REQUIRE(got == want);
        }
    }
}

TEST_CASE("audio path: blocks are seamless across the boundary", "[audio][lanes]") {
    const uint32_t bl     = lanes_test::boot();
    const uint64_t width  = framesFor(kDefaultWidthMs);
    const uint64_t period = framesFor(kDefaultPeriodMs);

    // Two consecutive blocks must continue one stream, not restart it. A DSP
    // whose phase reset per block would pass a single-block assertion and
    // click audibly every block; this is what catches that.
    const uint64_t a = lanes_test::tick();
    std::vector<float> first(clockwork_audio_out(), clockwork_audio_out() + bl * lanes_test::kOutChannels);
    const uint64_t b = lanes_test::tick();
    const float* second = clockwork_audio_out();

    REQUIRE(b == a + bl);
    for (uint32_t i = 0; i < bl; ++i) {
        REQUIRE(first[i]  == expectedSample(0, a + i, width, period));
        REQUIRE(second[i] == expectedSample(0, b + i, width, period));
    }
}

TEST_CASE("audio path: every pulse edge lands on its exact absolute frame",
          "[audio][lanes][timing]") {
    const uint64_t width  = framesFor(kDefaultWidthMs);
    const uint64_t period = framesFor(kDefaultPeriodMs);

    // Enough blocks to cover more than two full periods, so each channel is
    // seen switching on and off repeatedly rather than once by luck.
    const uint32_t bl     = lanes_test::boot();
    const int      blocks = static_cast<int>((period * 5 / 2) / bl) + 2;
    const Capture  cap    = capture(blocks);

    for (uint32_t ch = 0; ch < lanes_test::kOutChannels; ++ch) {
        const auto edges = edgesOf(cap, ch);

        int onsets = 0, ends = 0;
        for (const Edge& e : edges) {
            const uint64_t phase = e.frame % period;
            if (e.onset) {
                // A pulse begins exactly on a multiple of the period.
                REQUIRE(phase == 0);
                ++onsets;
            } else {
                // ...and ends exactly `width` frames later.
                REQUIRE(phase == width);
                ++ends;
            }
        }
        // Not vacuous: a channel stuck at one level has no edges at all and
        // would otherwise sail through the loop above.
        REQUIRE(onsets >= 2);
        REQUIRE(ends   >= 2);
    }
}

TEST_CASE("audio path: left and right are distinct channels", "[audio][lanes]") {
    REQUIRE(lanes_test::kOutChannels >= 2);
    const uint64_t period = framesFor(kDefaultPeriodMs);
    const uint32_t bl     = lanes_test::boot();

    // A full period, so the pulse falls inside the window rather than the test
    // resting on whatever level the gap happens to hold.
    const Capture cap = capture(static_cast<int>((period + bl - 1) / bl) + 2);

    int sounding = 0;
    for (std::size_t i = 0; i < cap.channel[0].size(); ++i) {
        const float l = cap.channel[0][i];
        const float r = cap.channel[1][i];

        // The exact invariant: same waveform, same instant, opposite sign. One
        // sample of skew between the channels spikes this to +/-2*amp at every
        // edge; a duplicated channel makes it 2*l; a lost sign makes it 2*l
        // too. Per-channel comparison against the formula catches none of
        // those on its own, because each channel still looks correct.
        REQUIRE(l + r == 0.0f);

        if (l != kOff) {
            // ...and left is the positive one, so a swap is visible.
            REQUIRE(l == kOn);
            REQUIRE(r == -kOn);
            ++sounding;
        }
    }
    // Not silence: a pair of muted channels satisfies l + r == 0 perfectly.
    REQUIRE(sounding > 0);
}

TEST_CASE("audio path: every sample is finite and is one of the two levels",
          "[audio][lanes]") {
    const uint32_t bl = lanes_test::boot();
    for (int block = 0; block < 8; ++block) {
        lanes_test::tick();
        const float* out = clockwork_audio_out();
        for (uint32_t i = 0; i < bl * lanes_test::kOutChannels; ++i) {
            REQUIRE(std::isfinite(out[i]));
            // Rectangular: exactly on (either polarity) or exactly off. A
            // value in between means something summed into the buffer, or read
            // the wrong region.
            REQUIRE((out[i] == kOn || out[i] == -kOn || out[i] == kOff));
        }
    }
}

TEST_CASE("audio path: an OSC message reaches the DSP and moves the edges",
          "[audio][lanes][timing][boundary]") {
    // The whole chain in one case: a message written to the IN ring, drained
    // and classified by clockwork, routed past "/clockwork/" to the default
    // route, handed to dsp_osc — and then OBSERVED in the samples, because the
    // pulse grid it reconfigures is the thing this file already knows how to
    // check to the frame. It is the case that would catch clockwork silently
    // dropping DSP-bound messages, which is not hypothetical: a host driving
    // the lanes ABI alone published no ingress root at all until this boundary was
    // narrowed, and every message it wrote was drained and discarded.
    const uint32_t bl = lanes_test::boot();

    constexpr uint32_t kNewWidthMs  = 5;
    constexpr uint32_t kNewPeriodMs = 100;
    const uint64_t width  = framesFor(kNewWidthMs);    // 240 @ 48k
    const uint64_t period = framesFor(kNewPeriodMs);   // 4800 @ 48k
    REQUIRE(period != framesFor(kDefaultPeriodMs));    // the test must be able to fail

    setPulse(kNewWidthMs, kNewPeriodMs);

    const Capture cap = capture(static_cast<int>((period * 5 / 2) / bl) + 2);
    for (uint32_t ch = 0; ch < lanes_test::kOutChannels; ++ch) {
        // Sample-exact against the NEW grid...
        for (std::size_t i = 0; i < cap.channel[ch].size(); ++i)
            REQUIRE(cap.channel[ch][i] ==
                    expectedSample(ch, cap.firstFrame + i, width, period));

        // ...and the edges land on the new frames, not the old ones.
        int onsets = 0;
        for (const Edge& e : edgesOf(cap, ch)) {
            const uint64_t phase = e.frame % period;
            REQUIRE(phase == (e.onset ? 0u : width));
            if (e.onset) ++onsets;
        }
        REQUIRE(onsets >= 2);
    }

    // Put the placeholder back the way the rest of the suite expects to find
    // it, and prove the restore took — the fixture is a process singleton, so
    // leaving it reconfigured would be leaving a trap for whatever runs next.
    setPulse(kDefaultWidthMs, kDefaultPeriodMs);
    const uint64_t at  = lanes_test::tick();
    const float*   out = clockwork_audio_out();
    for (uint32_t i = 0; i < bl; ++i)
        REQUIRE(out[i] == expectedSample(0, at + i,
                                         framesFor(kDefaultWidthMs),
                                         framesFor(kDefaultPeriodMs)));
}

TEST_CASE("audio path: the block size clockwork reports is the block it renders",
          "[audio][lanes]") {
    // clockwork_block_size() is what a host uses to size its own buffers, and it is
    // also what clockwork put in DspConfig::block_size and passes to
    // dsp_process as `frames`. A disagreement between them is silent
    // corruption at the join, so pin them together.
    const uint32_t bl = lanes_test::boot();
    REQUIRE(bl == clockwork_block_size());
    REQUIRE(bl == lanes_test::kBufLength);
}
