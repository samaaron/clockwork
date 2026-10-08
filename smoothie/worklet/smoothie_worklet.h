// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_worklet.h — the native audio worklet: what Smoothie offers the
 * code above it.
 *
 * A browser runs an engine inside an AudioWorklet: the platform owns the
 * hardware and calls the engine's processor with buffers, once per quantum.
 * Smoothie is that platform on macOS, Windows and Linux. JUCE — the drivers,
 * the devices, the platform's message loop — stays behind this header, and a
 * processor sees plain buffers and plain facts about the device.
 *
 * Nothing here includes JUCE, and nothing above Smoothie may: a host, the
 * engine and their tests reach the hardware through this header alone.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace smoothie {

// What a device opened with: everything a processor needs before its first
// block, as the driver reports it once the device is running.
struct DeviceInfo {
    std::string driver;                // "CoreAudio", "Windows Audio", "ALSA", ...
    std::string name;                  // the device's own name
    double      sampleRate = 0.0;
    int         bufferFrames = 0;      // frames per hardware callback
    int         inputChannels = 0;     // active
    int         outputChannels = 0;    // active
    int         inputLatencyFrames = 0;
    int         outputLatencyFrames = 0;

    struct Channel {
        std::string name;
        bool        active = false;
    };
    std::vector<Channel> outputs;      // every output the device has, in order
};

// When a block is, as far as the driver can say.
struct BlockTime {
    // The host clock at the block's first frame, in nanoseconds; 0 when the
    // driver supplies none, and the processor keeps its own clock instead.
    uint64_t hostTimeNs = 0;
};

// The processor a device runs: what an AudioWorkletProcessor is to a browser.
class Processor {
public:
    virtual ~Processor() = default;

    // Before a device's first block, on the thread that started the device.
    virtual void deviceStarting(const DeviceInfo& device) = 0;

    // Once per hardware callback, on the device's real-time thread, with
    // denormals flushed to zero. A device with no inputs passes null and 0.
    virtual void process(const float* const* inputs, int numInputs,
                         float* const* outputs, int numOutputs,
                         int frames, const BlockTime& time) = 0;

    // After a device's last block, on the thread that stopped the device.
    virtual void deviceStopped() = 0;
};

// Brings up what the platform's devices depend on: JUCE's message system.
// Once per process, from the first call, on the calling thread — which
// becomes its message thread — and never taken down: JUCE's singletons are
// not made to come back after they go, and every host before this held them
// for the life of the process anyway. Whatever opens a device calls this; a
// host need not.
void startPlatform();

}  // namespace smoothie
