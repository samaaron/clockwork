// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * RealDeviceGuard.h — no test opens a real audio device unless it says so.
 *
 * A case that boots an engine on the machine's own output changes that
 * device's sample rate and buffer size under every other app playing on
 * it: on 2026-10-16 one run of this suite started the built-in output 42
 * times and flipped the system device between 48 kHz and 44.1 kHz every
 * 1.5 s, with dropouts and clicks in whatever was playing. Nothing the
 * suite claims needs the hardware: cases run headless (EngineFixture) or on
 * the fake system (FakeAudioDevice.h), and the few that are ABOUT the
 * hardware carry the hidden [.hardware] tag and run only when named.
 *
 * The listener installs, for every case not tagged [.hardware], a process
 * default driver factory that REFUSES: the engine makes its device session's
 * drivers from it (ClockworkEngine::setDefaultDrivers), so a case that
 * reaches a real device fails on the throw, by name, before any device is
 * touched. Registered from test_main.cpp.
 */
#pragma once
#include "ClockworkEngine.h"
#include "smoothie_juce_drivers.h"
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <memory>
#include <stdexcept>
#include <string>

namespace real_device_guard {

inline std::shared_ptr<const smoothie::DriverFactory> refusing(const std::string& caseName) {
    auto f = std::make_shared<smoothie::DriverFactory>();
    f->make = [caseName]() -> smoothie::Drivers {
        throw std::runtime_error(
            "\"" + caseName + "\" would open a real audio device. Run headless "
            "(EngineFixture::defaultConfig()), on the fake system "
            "(FakeAudioDevice.h), or tag the case [.hardware] if the hardware "
            "is its subject (test/RealDeviceGuard.h).");
    };
    return f;
}

struct Listener : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(Catch::TestCaseInfo const& info) override {
        // Catch renders the hidden tag [.hardware] as [.] and [hardware].
        const bool hardware = info.tagsAsString().find("[hardware]") != std::string::npos;
        ClockworkEngine::setDefaultDrivers(hardware ? nullptr : refusing(info.name));
    }
    void testCaseEnded(Catch::TestCaseStats const&) override {
        ClockworkEngine::setDefaultDrivers(refusing("(between cases)"));
    }
};

} // namespace real_device_guard
