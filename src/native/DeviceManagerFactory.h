// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * DeviceManagerFactory.h — what ClockworkEngine::Config::deviceManagerFactory
 * holds: how to build the engine's device manager in place of a plain one.
 *
 * A test's seam (test/FakeAudioDevice.h). It names JUCE's manager, so it
 * lives here rather than in ClockworkEngine.h, which a host includes and
 * which must not need JUCE.
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <functional>
#include <memory>

struct DeviceManagerFactory {
    std::function<std::unique_ptr<juce::AudioDeviceManager>()> make;
};
