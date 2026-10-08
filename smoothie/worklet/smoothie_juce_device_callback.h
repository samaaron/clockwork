// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_juce_device_callback.h — a worklet Processor as the device
 * callback JUCE calls.
 *
 * Smoothie's own side of the worklet: the facts and buffers JUCE hands a
 * device callback, turned into the plain ones a Processor takes. It includes
 * JUCE, so only code that holds a JUCE device manager uses it, and that code
 * belongs in Smoothie.
 */
#pragma once

#include "smoothie_worklet.h"
#include <juce_audio_devices/juce_audio_devices.h>

namespace smoothie {

class JuceDeviceCallback final : public juce::AudioIODeviceCallback {
public:
    explicit JuceDeviceCallback(Processor& processor) : mProcessor(processor) {}

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs,
                                          float* const* outputs, int numOutputs,
                                          int frames,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceStopped() override;

    // What a running device reports about itself, as a Processor takes it.
    static DeviceInfo describe(juce::AudioIODevice& device);

private:
    Processor& mProcessor;
};

}  // namespace smoothie
