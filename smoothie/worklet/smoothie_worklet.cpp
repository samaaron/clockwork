// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_worklet.cpp — the worklet's JUCE side: starting the platform, and
 * the device callback that hands a Processor its buffers.
 */
#include "smoothie_juce_device_callback.h"

#include <juce_events/juce_events.h>
#include <mutex>

namespace smoothie {

void startPlatform() {
    static std::once_flag once;
    std::call_once(once, [] { juce::initialiseJuce_GUI(); });
}

DeviceInfo JuceDeviceCallback::describe(juce::AudioIODevice& device) {
    DeviceInfo d;
    d.driver              = device.getTypeName().toStdString();
    d.name                = device.getName().toStdString();
    d.sampleRate          = device.getCurrentSampleRate();
    d.bufferFrames        = device.getCurrentBufferSizeSamples();
    d.inputChannels       = device.getActiveInputChannels().countNumberOfSetBits();
    d.outputChannels      = device.getActiveOutputChannels().countNumberOfSetBits();
    d.inputLatencyFrames  = device.getInputLatencyInSamples();
    d.outputLatencyFrames = device.getOutputLatencyInSamples();
    const auto names  = device.getOutputChannelNames();
    const auto active = device.getActiveOutputChannels();
    d.outputs.reserve(static_cast<size_t>(names.size()));
    for (int i = 0; i < names.size(); ++i)
        d.outputs.push_back({ names[i].toStdString(), active[i] });
    return d;
}

void JuceDeviceCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    mProcessor.deviceStarting(describe(*device));
}

void JuceDeviceCallback::audioDeviceIOCallbackWithContext(
    const float* const* inputs, int numInputs,
    float* const* outputs, int numOutputs,
    int frames, const juce::AudioIODeviceCallbackContext& context) {
    // Denormal flush-to-zero is a per-thread flag, and this is the thread
    // that renders: armed here, every block of every device has it.
    const juce::ScopedNoDenormals noDenormals;
    BlockTime time;
    if (context.hostTimeNs != nullptr) time.hostTimeNs = *context.hostTimeNs;
    mProcessor.process(inputs, numInputs, outputs, numOutputs, frames, time);
}

void JuceDeviceCallback::audioDeviceStopped() {
    mProcessor.deviceStopped();
}

}  // namespace smoothie
