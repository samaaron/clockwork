// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_juce_drivers.h — the drivers a DeviceSession is made of.
 *
 * The session's JUCE side (smoothie_devices.h is the rest): a driver is a
 * juce::AudioIODeviceType. The platform's are JUCE's own native types; a
 * caller that wants others — a driver Smoothie does not carry, a test's
 * fakes — hands the session a factory that makes them. It includes JUCE, so
 * only code that makes drivers uses it.
 */
#pragma once

#include "smoothie_devices.h"
#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <memory>
#include <vector>

namespace smoothie {

using Drivers = std::vector<std::unique_ptr<juce::AudioIODeviceType>>;

// What a session's drivers are made by. Called once for each session, so a
// session made afresh (a recovery's) has drivers of its own.
struct DriverFactory {
    std::function<Drivers()> make;
};

// The platform's native drivers, in the order a session prefers them when
// nothing else decides — JUCE's own: WASAPI's three modes, DirectSound and
// ASIO on Windows; CoreAudio on macOS; iOS's; Bela, ALSA and JACK on Linux;
// Oboe, OpenSL and Java audio on Android. A driver this platform or build
// leaves out is not there.
Drivers platformDrivers();

}  // namespace smoothie
