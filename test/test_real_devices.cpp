// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_real_devices.cpp — the engine on this machine's own audio devices.
 *
 * Opt-in, and never in CI: the tag is hidden. Run it at a machine whose
 * devices are attached and switched on:
 *
 *     ./build/test/clockwork_engine_tests "[hardware]" -s
 *
 * Nothing is played (every output gets silence), but the default microphone
 * is recorded from — the OS may ask once for permission, and without it the
 * input checks read silence. Each output the driver lists is opened in turn;
 * the WARN lines say what was found and how long each switch took.
 *
 * What the fakes can't show: that the driver really pairs a separate
 * microphone with each output (CoreAudio through an aggregate device), that
 * the microphone's own channels are the ones recorded, and that no
 * aggregate is left behind — this run's, or one an earlier crash left.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "JuceAudioCallback.h"   // get_audio_buffer_samples
#include "lanes/lanes.h"         // clockwork_audio_in
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

ClockworkEngine::Config realConfig() {
    ClockworkEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.udpPort           = 0;
    cfg.headless          = false;
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 1;   // the default microphone, paired at boot
    cfg.appName           = "ClockworkHardwareTest";
    return cfg;
}

// The loudest sample on the first input channel over `ms`. A microphone in a
// room is never exactly silent; a channel nothing feeds (a virtual device's
// own returns, which a wrong channel offset would record) is.
float inputPeak(int ms) {
    float peak = 0.0f;
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (const float* in = clockwork_audio_in()) {
            const int block = get_audio_buffer_samples();
            for (int i = 0; i < block; ++i) peak = std::max(peak, std::fabs(in[i]));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return peak;
}

// CoreAudio takes a moment to drop a destroyed aggregate from its list.
template <typename Pred>
bool within(int ms, Pred pred) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (Clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

long msSince(Clock::time_point t) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

#ifdef __APPLE__
std::vector<std::string> deviceUIDs() {
    std::vector<std::string> uids;
    AudioObjectPropertyAddress devices { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, nullptr, &size) != noErr)
        return uids;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices, 0, nullptr, &size, ids.data()) != noErr)
        return uids;
    for (auto id : ids) {
        AudioObjectPropertyAddress uidAddress { kAudioDevicePropertyDeviceUID,
                                                kAudioObjectPropertyScopeGlobal,
                                                kAudioObjectPropertyElementMain };
        CFStringRef uid = nullptr;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(id, &uidAddress, 0, nullptr, &uidSize, &uid) != noErr || !uid)
            continue;
        char buf[512] = {};
        CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(uid);
        uids.emplace_back(buf);
    }
    return uids;
}

// The rate the HAL says a device runs at now, by name; 0 if it isn't there.
double nominalRate(const std::string& name) {
    AudioObjectPropertyAddress devices { kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devices, 0, nullptr, &size) != noErr)
        return 0;
    std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devices, 0, nullptr, &size, ids.data()) != noErr)
        return 0;
    for (auto id : ids) {
        AudioObjectPropertyAddress nameAddress { kAudioObjectPropertyName,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof(cfName);
        if (AudioObjectGetPropertyData(id, &nameAddress, 0, nullptr, &nameSize, &cfName) != noErr || !cfName)
            continue;
        char buf[512] = {};
        CFStringGetCString(cfName, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(cfName);
        if (name != buf) continue;
        AudioObjectPropertyAddress rateAddress { kAudioDevicePropertyNominalSampleRate,
                                                 kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
        Float64 rate = 0;
        UInt32 rateSize = sizeof(rate);
        AudioObjectGetPropertyData(id, &rateAddress, 0, nullptr, &rateSize, &rate);
        return rate;
    }
    return 0;
}

// Aggregate devices this process has made now.
int ourAggregates() {
    const std::string prefix = "smoothie.aggregate." + std::to_string(getpid()) + ".";
    int n = 0;
    for (auto& uid : deviceUIDs()) n += uid.rfind(prefix, 0) == 0;
    return n;
}

// Aggregates an earlier engine left: smoothie's of a process that is gone,
// or one named the ways engines named them before smoothie made them.
std::vector<std::string> leftoverAggregates() {
    std::vector<std::string> left;
    const std::string mine = "smoothie.aggregate." + std::to_string(getpid()) + ".";
    for (auto& uid : deviceUIDs()) {
        const bool legacy = ((uid.rfind("clockwork.", 0) == 0 || uid.rfind("net.sonic-pi.", 0) == 0)
                             && uid.find(".aggregate") != std::string::npos)
                         || uid.rfind("com.sonicpi.supersonic.aggregate", 0) == 0;
        const bool smoothies = uid.rfind("smoothie.aggregate.", 0) == 0 && uid.rfind(mine, 0) != 0;
        if (legacy || smoothies) left.push_back(uid);
    }
    return left;
}
#endif

}  // namespace

TEST_CASE("Hardware: boots on the default output with the default microphone, records "
          "from it, and leaves no aggregate behind", "[.hardware]") {
#ifdef __APPLE__
    const auto before = leftoverAggregates();
    for (auto& uid : before) WARN("leftover aggregate before boot: " << uid);
#endif
    {
        EngineFixture fix(realConfig());
        const auto cur = fix.engine().currentDevice();
        INFO(fix.debugMessagesDump());
        REQUIRE_FALSE(cur.name.empty());
        WARN("booted on '" << cur.name << "' (" << cur.typeName << ") at "
             << cur.activeSampleRate << " Hz, buffer " << cur.activeBufferSize
             << ", recording from '" << cur.inputDeviceName << "'");
        CHECK(cur.activeInputChannels > 0);
        CHECK(fix.waitForBlocks(200, 5000));
        const float peak = inputPeak(1000);
        WARN("microphone peak over 1 s: " << peak);
        CHECK(peak > 0.0f);   // zero: no microphone permission, or the wrong channels
#ifdef __APPLE__
        // Leftovers go when the driver comes up — unless an older app plays on one.
        for (auto& uid : leftoverAggregates()) WARN("leftover aggregate after boot: " << uid);
        const bool paired = !cur.inputDeviceName.empty() && cur.inputDeviceName != cur.name;
        CHECK(ourAggregates() == (paired ? 1 : 0));
        for (auto& d : fix.engine().listDevices())
            CHECK(d.name.find("ClockworkHardwareTest#") == std::string::npos);
#endif
    }
#ifdef __APPLE__
    CHECK(within(2000, [] { return ourAggregates() == 0; }));
#endif
}

TEST_CASE("Hardware: the microphone plays with every output that pairs with it, sits "
          "out with any that doesn't, and comes back after", "[.hardware]") {
    EngineFixture fix(realConfig());
    const auto first = fix.engine().currentDevice();
    const std::string mic = first.inputDeviceName;
    REQUIRE_FALSE(mic.empty());

    for (auto& d : fix.engine().listDevices(true)) {
        if (d.maxOutputChannels <= 0 || d.typeName != first.typeName) continue;
        if (d.wireless) {
            WARN("'" << d.name << "': wireless, opened only as the system default — skipped");
            continue;
        }
        INFO("output '" << d.name << "'");
        fix.clearDebugMessages();
        const auto t0 = Clock::now();
        const auto r = switchWhenFree(fix, d.name);
        const long switchMs = msSince(t0);
        INFO(fix.debugMessagesDump());
        CHECK(r.success);
        if (!r.success) {
            WARN("'" << d.name << "': switch failed — " << r.error);
            continue;
        }
        const auto cur = fix.engine().currentDevice();
        CHECK(cur.name == d.name);
        CHECK(fix.waitForBlocks(100, 5000));
        const long playingMs = msSince(t0);
        if (d.pairs) {
            CHECK(cur.inputDeviceName == mic);
            CHECK(cur.activeInputChannels > 0);
            const float peak = inputPeak(500);
            CHECK(peak > 0.0f);   // the microphone's channels, not the output's own inputs
#ifdef __APPLE__
            CHECK(within(2000, [&] { return ourAggregates() == (d.name == mic ? 0 : 1); }));
#endif
            WARN("'" << d.name << "' + '" << cur.inputDeviceName << "': switched in "
                 << switchMs << " ms, playing after " << playingMs << " ms, "
                 << cur.activeSampleRate << " Hz, buffer " << cur.activeBufferSize
                 << ", mic peak " << peak);
        } else {
            CHECK_FALSE(r.inputUnavailable);   // set aside, not refused by the driver
            CHECK(cur.activeInputChannels == 0);
#ifdef __APPLE__
            CHECK(within(2000, [] { return ourAggregates() == 0; }));
#endif
            WARN("'" << d.name << "': doesn't pair — playing alone after " << playingMs << " ms");
        }
    }

    // Back where it started, the microphone with it.
    fix.clearDebugMessages();
    const auto back = switchWhenFree(fix, first.name);
    INFO(fix.debugMessagesDump());
    CHECK(back.success);
    const auto cur = fix.engine().currentDevice();
    CHECK(cur.name == first.name);
    CHECK(cur.inputDeviceName == mic);
    CHECK(cur.activeInputChannels > 0);
    CHECK(inputPeak(500) > 0.0f);
}

// A device can refuse a rate it lists (an interface whose clock is set on the
// box, or by its own app). The pair then runs at the rate its output really
// has, both halves at it, and says that rate — never the one asked for.
TEST_CASE("Hardware: an output and a separate microphone change rate together, "
          "and the engine says the rate they really run at", "[.hardware]") {
    EngineFixture fix(realConfig());
    const auto first = fix.engine().currentDevice();
    if (first.inputDeviceName.empty() || first.inputDeviceName == first.name) {
        WARN("the default output and microphone are one device — nothing to pair");
        return;
    }
    for (const double rate : { 44100.0, 48000.0 }) {
        INFO(rate << " Hz");
        fix.clearDebugMessages();
        const auto t0 = Clock::now();
        const auto r = switchWhenFree(fix, first.name, rate, 0, false, first.inputDeviceName);
        INFO(fix.debugMessagesDump());
        CHECK(r.success);
        const auto cur = fix.engine().currentDevice();
        CHECK(cur.inputDeviceName == first.inputDeviceName);
        CHECK(fix.waitForBlocks(100, 5000));
        const float peak = inputPeak(500);
        CHECK(peak > 0.0f);
#ifdef __APPLE__
        const double outRate = nominalRate(cur.name);
        const double inRate  = nominalRate(cur.inputDeviceName);
        CHECK(cur.activeSampleRate == outRate);
        CHECK(inRate == outRate);   // one clock: no resampling inside the pair
        if (outRate != rate)
            WARN("'" << cur.name << "' stays at " << outRate << " Hz when asked for " << rate);
#else
        CHECK(cur.activeSampleRate == rate);
#endif
        WARN(rate << " Hz: switched in " << msSince(t0) << " ms, running at "
             << cur.activeSampleRate << " Hz, buffer " << cur.activeBufferSize
             << ", mic peak " << peak);
    }
}

