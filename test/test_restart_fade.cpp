// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_fade.cpp — a restart is heard as a dip, never as a click.
 *
 * Every restart stops the device and starts it again: a buffer change, a rate
 * change, a switch, an input turned on or off. Stopped mid-waveform, the
 * output steps from wherever the wave was to silence, and started again it
 * steps back: two clicks each time, heard on every restart in Sonic Pi
 * (2026-10-17). These listen to what a fake device plays across restarts,
 * with the dummy guest's 440 Hz tone, and hold every step to what the tone
 * makes on its own.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using fake_audio::FakeDeviceSpec;
using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

// The dummy guest's tone (dummy_dsp.cpp): 0.2 at 440 Hz. Its largest step
// from one sample to the next, at `rate`, with room for the fade's own slope.
double toneStep(double rate) { return 0.2 * 6.283185307179586 * 440.0 / rate; }
double clickBound(double rate) { return 1.5 * toneStep(rate); }

struct Heard {
    std::vector<float>  samples;
    std::vector<size_t> stops;
};

Heard heard(FakeDeviceSpec& device) {
    std::lock_guard<std::mutex> lock(device.played->m);
    return { device.played->samples, device.played->stops };
}

// Where to listen from: a little before now, inside the tone already playing,
// so a restart that stops the device at once is still heard stopping it.
size_t listenFrom(FakeDeviceSpec& device) {
    std::lock_guard<std::mutex> lock(device.played->m);
    constexpr size_t kBefore = 1024;
    REQUIRE(device.played->samples.size() > kBefore);
    return device.played->samples.size() - kBefore;
}

// The largest step a listener hears from `from` on: sample to sample, and
// into the silence each stop leaves and out of it again.
double largestStep(const Heard& h, size_t from) {
    double worst = 0;
    float  prev  = from < h.samples.size() ? h.samples[from] : 0.0f;
    auto   stop  = std::upper_bound(h.stops.begin(), h.stops.end(), from);
    for (size_t i = from + 1; i < h.samples.size(); ++i) {
        for (; stop != h.stops.end() && *stop == i; ++stop) {
            worst = std::max(worst, static_cast<double>(std::abs(prev)));
            prev = 0.0f;
        }
        worst = std::max(worst, static_cast<double>(std::abs(h.samples[i] - prev)));
        prev = h.samples[i];
    }
    return worst;
}

// The dummy guest playing its tone, heard: the mode is the guest's, set by
// the time its pong answers.
void playTone(EngineFixture& fix) {
    fix.send(osc_test::message("/dummy/tone"));
    fix.send(osc_test::message("/dummy/ping"));
    OscReply pong;
    REQUIRE(fix.waitForReply("/dummy/pong", pong));
    REQUIRE(fix.waitForBlocks(20));
}

}  // namespace

TEST_CASE("RestartFade: a hot restart fades out before the device stops and in after it starts",
          "[RestartFade]") {
    auto sys = makeSimpleSystem();
    auto& speakers = sys->at("Fake Speakers");
    speakers.played = std::make_shared<FakeDeviceSpec::Played>();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    playTone(fix);
    const size_t from = listenFrom(speakers);

    // Three buffer changes, each a hot restart of the same device: three
    // stops and three starts, at three unrelated points in the wave.
    for (const int buffer : { 256, 512, 128 }) {
        const auto r = fix.engine().switchDevice("", 0, buffer);
        INFO(fix.debugMessagesDump());
        REQUIRE(r.success);
        REQUIRE(r.type == SwapType::Hot);
        REQUIRE(fix.waitForBlocks(40));
    }

    const Heard h = heard(speakers);
    REQUIRE(std::count_if(h.stops.begin(), h.stops.end(), [&](size_t s) { return s > from; }) == 3);
    INFO("largest step " << largestStep(h, from) << ", the tone's own " << toneStep(48000));
    CHECK(largestStep(h, from) <= clickBound(48000));
}

TEST_CASE("RestartFade: a cold restart fades out before the instance goes and in when it is back",
          "[RestartFade]") {
    // A rate change rebuilds the guest, which comes back on its pulse: edges
    // of its own. What is held here is the restart's: the last of the tone
    // before the silence, and the first of what follows it.
    auto sys = makeSimpleSystem();
    auto& speakers = sys->at("Fake Speakers");
    speakers.played = std::make_shared<FakeDeviceSpec::Played>();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    playTone(fix);
    const size_t from = listenFrom(speakers);

    const auto r = fix.engine().switchDevice("", 44100, 0);
    INFO(fix.debugMessagesDump());
    REQUIRE(r.success);
    REQUIRE(r.type == SwapType::Cold);
    REQUIRE(fix.waitForBlocks(40));

    const Heard h = heard(speakers);
    const auto stop = std::upper_bound(h.stops.begin(), h.stops.end(), from);
    REQUIRE(stop != h.stops.end());
    // Into the silence: the last sound before the stop.
    size_t last = *stop;
    while (last > from && h.samples[last - 1] == 0.0f) --last;
    REQUIRE(last > from);
    CHECK(std::abs(h.samples[last - 1]) <= clickBound(48000));
    // Out of it: the first sound after the start.
    size_t first = *stop;
    while (first < h.samples.size() && h.samples[first] == 0.0f) ++first;
    REQUIRE(first < h.samples.size());
    CHECK(std::abs(h.samples[first]) <= clickBound(44100));
}
