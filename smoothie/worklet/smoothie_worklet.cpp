// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
/*
 * smoothie_worklet.cpp — the worklet's JUCE side: starting the platform. The
 * device callback that hands a Processor its buffers is the device
 * session's (smoothie_devices.cpp).
 */
#include "smoothie_worklet.h"

#include <juce_events/juce_events.h>
#include <mutex>

namespace smoothie {

void startPlatform() {
    static std::once_flag once;
    std::call_once(once, [] { juce::initialiseJuce_GUI(); });
}

}  // namespace smoothie
