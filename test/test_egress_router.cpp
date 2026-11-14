// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_egress_router.cpp — one egress frame to the client, or to the host's
 * log (src/comms/EgressRouter.h). Everything on the egress is for the one
 * client, and /clockwork/debug is a log line rather than a message. The
 * engine's own gateway and a host draining egress itself come through the
 * same function, so this is the one place the rule is pinned.
 */
#include <catch2/catch_test_macros.hpp>

#include "EgressRouter.h"
#include "OscTestUtils.h"

#include <string>
#include <vector>

namespace {

struct Sink final : IOscTransport {
    std::vector<std::string> log;
    bool delivers = true;
    bool deliver(const uint8_t* d, uint32_t n) override { log.push_back(osc_test::parseAddress(d, n)); return delivers; }
};

} // namespace

TEST_CASE("egress router: every frame is handed to the client", "[egress][router]") {
    Sink sink;
    std::vector<std::string> debug;
    auto onDebug = [&](const std::string& s) { debug.push_back(s); };
    for (const char* address : { "/x", "/clockwork/statechange", "/clockwork/midi/in/note_on" }) {
        const auto m = osc_test::message(address);
        CHECK(clockwork_route_egress(sink, m.ptr(), m.size(), onDebug) == 0);
    }
    const std::vector<std::string> expect = { "/x", "/clockwork/statechange", "/clockwork/midi/in/note_on" };
    CHECK(sink.log == expect);
    CHECK(debug.empty());
}

TEST_CASE("egress router: what the transport could not deliver comes back as a count", "[egress][router]") {
    // There is one client, so a frame it could not be handed misses one. The
    // caller counts them (egress_frames_undelivered).
    Sink sink;
    sink.delivers = false;
    const auto m = osc_test::message("/x");
    CHECK(clockwork_route_egress(sink, m.ptr(), m.size(), {}) == 1);
    // A log line is no client's message, whether or not a log takes it.
    const auto debug = osc_test::message("/clockwork/debug", "a line");
    CHECK(clockwork_route_egress(sink, debug.ptr(), debug.size(), {}) == 0);
}

TEST_CASE("egress router: /clockwork/debug is a log line, not a message", "[egress][router]") {
    Sink sink;
    std::vector<std::string> debug;
    const auto m = osc_test::message("/clockwork/debug", "hello from the engine");
    CHECK(clockwork_route_egress(sink, m.ptr(), m.size(),
                                 [&](const std::string& s) { debug.push_back(s); }) == 0);
    CHECK(sink.log.empty());
    REQUIRE(debug.size() == 1);
    CHECK(debug[0] == "hello from the engine");
    // Without a log to go to, it goes nowhere rather than out as a message.
    CHECK(clockwork_route_egress(sink, m.ptr(), m.size(), {}) == 0);
    CHECK(sink.log.empty());
}
