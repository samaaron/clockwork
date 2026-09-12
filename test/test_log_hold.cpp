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
#include "ClockworkEngine.h"
#include "clockwork_config.h"
#include "clockwork_event_sink.h"
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct DebugCapture {
    std::mutex mu;
    std::vector<std::string> lines;
    bool has(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& l : lines)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    int indexOf(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find(needle) != std::string::npos) return static_cast<int>(i);
        return -1;
    }
};

template <typename Pred>
bool pollUntil(Pred pred, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

} // namespace

TEST_CASE("a line logged before the ring exists reaches the debug channel once the engine is up",
          "[log][boot]") {
    // What init() does first — hold — then a line with no ring to go to.
    clockwork_log_hold();
    clockwork_log("[test] logged before the ring existed");

    DebugCapture cap;
    ClockworkEngine engine;
    engine.onDebug = [&cap](const std::string& s) {
        std::lock_guard<std::mutex> lk(cap.mu);
        cap.lines.push_back(s);
    };
    engine.onReply = [](const uint8_t*, uint32_t) {};

    auto cfg = EngineFixture::defaultConfig();
    cfg.hostDrivesControl = false;   // the engine's own gateway drains the ring
    engine.init(cfg);

    // The DSP identity line is logged straight onto the ring right after it
    // is built; the held line must be on the channel too, and before it.
    REQUIRE(pollUntil([&] { return cap.has("DSP: "); }));
    INFO("debug lines seen:" << [&] {
        std::string out; std::lock_guard<std::mutex> lk(cap.mu);
        for (auto& l : cap.lines) out += "\n  | " + l; return out; }());
    CHECK(cap.has("[test] logged before the ring existed"));
    CHECK(cap.indexOf("[test] logged before the ring existed") < cap.indexOf("DSP: "));

    engine.shutdown();
    clockwork_sink_close_all();
}
