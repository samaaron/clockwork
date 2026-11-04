// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// clockwork_log before the debug ring exists.
//
// Contract: a line logged while an engine is booting, before initEngine has
// built the ring, is not lost and not diverted to stderr — it is held and
// replayed onto the ring as soon as the ring is up, ahead of everything
// logged after, so the host's debug channel carries the whole boot story
// (device setup included) in order.
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "clockwork_config.h"
#include "clockwork_core.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("a line logged before the ring exists reaches the debug channel once the engine is up",
          "[log][boot]") {
    // A line of the booting core's log, said while the engine boots, before
    // its ring exists: from the device lane, as the boot opens its device.
    // The core holds it. The core is made here and handed to the engine
    // (Config::core), so the line can name it before the engine exists.
    ClockworkStatus made = CLOCKWORK_OK;
    ClockworkCore* const core = clockwork_core_new(&made);
    REQUIRE(core != nullptr);
    auto sys = fake_audio::makeSimpleSystem();
    sys->at("Fake Speakers").onOpen = [core] { clockwork_log(core, "[test] logged before the ring existed"); };
    // The fixture keeps what arrives after the boot; the boot's own lines
    // are this case's, so it listens from before init.
    std::mutex mu;
    std::vector<std::string> lines;
    ClockworkEngine::Config cfg = fake_audio::fakeEngineConfig(sys, "Fake Speakers");
    cfg.core = core;
    EngineFixture fix(cfg, [&](ClockworkEngine& e) {
        e.onDebug = [&, theFixtures = e.onDebug](const std::string& line) {
            theFixtures(line);
            const std::lock_guard<std::mutex> lk(mu);
            lines.push_back(line);
        };
    });

    // The DSP identity line is logged straight onto the ring right after it
    // is built; the held line must be on the channel too, and before it.
    const auto at = [&](const std::string& needle) {
        const std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find(needle) != std::string::npos) return static_cast<int>(i);
        return -1;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (at("DSP: ") < 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    INFO("debug lines seen:" << [&] {
        std::string out; const std::lock_guard<std::mutex> lk(mu);
        for (const auto& l : lines) out += "\n  | " + l; return out; }());
    REQUIRE(at("DSP: ") >= 0);
    CHECK(at("[test] logged before the ring existed") >= 0);
    CHECK(at("[test] logged before the ring existed") < at("DSP: "));
}
