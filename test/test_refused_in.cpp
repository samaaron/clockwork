// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_refused_in.cpp — a message refused on its way in is counted
 * (PerformanceMetrics::messages_dropped) and said in the log, by its address
 * and why, at most once a second (ClockworkEngine::noteRefusedIn): the count
 * alone says neither when nor which.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "shared_memory.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

int linesSaying(EngineFixture& fix, const std::string& text) {
    int n = 0;
    for (const auto& line : fix.debugMessages())
        if (line.find(text) != std::string::npos) ++n;
    return n;
}

}  // namespace

TEST_CASE("A message refused on its way in is counted and said, by its address and why", "[refused][engine]") {
    EngineFixture fix;
    const auto& m = fix.engine().getMetrics();
    const uint32_t before = m.messages_dropped.load();

    // Wider than the IN ring: refused whole.
    std::vector<uint8_t> wide(clockwork::kDefaultLayout.profile.in_ring_bytes + 64, 0);
    const char address[] = "/too/wide";
    std::memcpy(wide.data(), address, sizeof address);
    CHECK(fix.engine().ingest(wide.data(), static_cast<uint32_t>(wide.size())) != CLOCKWORK_OK);
    CHECK(m.messages_dropped.load() == before + 1);
    REQUIRE(fix.pollUntil([&] { return linesSaying(fix, "a message was refused on its way in: /too/wide") == 1; }));

    // Again within the second: counted, not said again.
    CHECK(fix.engine().ingest(wide.data(), static_cast<uint32_t>(wide.size())) != CLOCKWORK_OK);
    CHECK(m.messages_dropped.load() == before + 2);
    fix.pollUntil([] { return false; }, 200);   // what the control pass would carry, carried
    CHECK(linesSaying(fix, "a message was refused on its way in") == 1);
}
