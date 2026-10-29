// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_egress_router.cpp — one egress frame to one transport call
 * (src/comms/EgressRouter.h): the route word decides, the origin says to
 * whom, and /clockwork/debug is a log line rather than a message. The
 * engine's own gateway and a host draining egress itself route through the
 * same function, so this is the one place the table is pinned.
 */
#include <catch2/catch_test_macros.hpp>

#include "EgressRouter.h"
#include "OscTestUtils.h"
#include "shared_memory.h"

#include <string>
#include <vector>

namespace {

struct Sink final : IOscTransport {
    std::vector<std::string> log;
    uint32_t lastToken = 0;
    bool lastNetworkOnly = false;
    bool delivers = true;    // what send says
    uint32_t misses = 0;     // what every broadcast says it missed
    bool send(uint32_t token, const uint8_t* d, uint32_t n, bool networkOnly) override {
        lastToken = token; lastNetworkOnly = networkOnly;
        log.push_back("send " + osc_test::parseAddress(d, n)); return delivers;
    }
    uint32_t broadcastNotify(const uint8_t* d, uint32_t n) override { log.push_back("notify " + osc_test::parseAddress(d, n)); return misses; }
    uint32_t broadcastLink(const uint8_t* d, uint32_t n) override { log.push_back("link " + osc_test::parseAddress(d, n)); return misses; }
    bool hasNotifySubscribers() const override { return true; }
    bool subscribeNotify(uint32_t) override { return true; }
    void unsubscribeNotify(uint32_t) override {}
    void clearNotify() override {}
    bool subscribeLink(uint32_t) override { return true; }
    void unsubscribeLink(uint32_t) override {}
    uint32_t broadcastMidi(const uint8_t* d, uint32_t n) override { log.push_back("midi " + osc_test::parseAddress(d, n)); return misses; }
    bool subscribeMidi(uint32_t) override { return true; }
    void unsubscribeMidi(uint32_t) override {}
    uint32_t broadcastGamepad(const uint8_t* d, uint32_t n) override { log.push_back("gamepad " + osc_test::parseAddress(d, n)); return misses; }
    bool subscribeGamepad(uint32_t) override { return true; }
    void unsubscribeGamepad(uint32_t) override {}
    uint32_t broadcastOsc(const uint8_t* d, uint32_t n) override { log.push_back("osc " + osc_test::parseAddress(d, n)); return misses; }
    bool subscribeOsc(uint32_t) override { return true; }
    void unsubscribeOsc(uint32_t) override {}
};

} // namespace

TEST_CASE("egress router: the route word picks the transport call, the origin says to whom", "[egress][router]") {
    Sink sink;
    std::vector<std::string> debug;
    auto onDebug = [&](const std::string& s) { debug.push_back(s); };
    const auto m = osc_test::message("/x");
    auto route = [&](uint32_t token, uint32_t r) { return clockwork_route_egress(sink, token, r, m.ptr(), m.size(), onDebug); };

    CHECK(route(7, EGRESS_REPLY) == 0);
    CHECK(sink.lastToken == 7);
    CHECK_FALSE(sink.lastNetworkOnly);
    CHECK(route(8, EGRESS_SEND_TO_CALLER) == 0);
    CHECK(sink.lastToken == 8);
    CHECK(sink.lastNetworkOnly);
    CHECK(route(0, EGRESS_BROADCAST_NOTIFY) == 0);
    CHECK(route(0, EGRESS_BROADCAST_LINK) == 0);
    CHECK(route(0, EGRESS_BROADCAST_MIDI) == 0);
    CHECK(route(0, EGRESS_BROADCAST_GAMEPAD) == 0);
    CHECK(route(0, EGRESS_BROADCAST_OSC) == 0);
    const std::vector<std::string> expect = { "send /x", "send /x", "notify /x", "link /x", "midi /x", "gamepad /x", "osc /x" };
    CHECK(sink.log == expect);
    CHECK(debug.empty());

    // A route nobody defined goes to the notify audience, as the engine's
    // gateway always sent it, rather than being lost.
    CHECK(route(0, 99) == 0);
    CHECK(sink.log.back() == "notify /x");
}

TEST_CASE("egress router: what the transport could not deliver comes back as a count", "[egress][router]") {
    // A reply the transport could not hand to its origin misses one; a
    // broadcast misses each member of its audience it did not reach. The
    // caller counts them (egress_frames_undelivered).
    Sink sink;
    sink.delivers = false;
    sink.misses = 2;
    const auto m = osc_test::message("/x");
    auto route = [&](uint32_t token, uint32_t r) { return clockwork_route_egress(sink, token, r, m.ptr(), m.size(), {}); };
    CHECK(route(7, EGRESS_REPLY) == 1);
    CHECK(route(7, EGRESS_SEND_TO_CALLER) == 1);
    for (uint32_t r : { EGRESS_BROADCAST_NOTIFY, EGRESS_BROADCAST_LINK, EGRESS_BROADCAST_MIDI,
                        EGRESS_BROADCAST_GAMEPAD, EGRESS_BROADCAST_OSC }) {
        INFO("route " << r);
        CHECK(route(0, r) == 2);
    }
    // A log line is no client's message, whether or not a log takes it.
    const auto debug = osc_test::message("/clockwork/debug", "a line");
    CHECK(clockwork_route_egress(sink, 0, EGRESS_BROADCAST_NOTIFY, debug.ptr(), debug.size(), {}) == 0);
}

TEST_CASE("egress router: /clockwork/debug is a log line, not a message", "[egress][router]") {
    Sink sink;
    std::vector<std::string> debug;
    const auto m = osc_test::message("/clockwork/debug", "hello from the engine");
    CHECK(clockwork_route_egress(sink, 0, EGRESS_BROADCAST_NOTIFY, m.ptr(), m.size(),
                                 [&](const std::string& s) { debug.push_back(s); }) == 0);
    CHECK(sink.log.empty());
    REQUIRE(debug.size() == 1);
    CHECK(debug[0] == "hello from the engine");
    // Without a log to go to, it goes nowhere rather than out as a message.
    CHECK(clockwork_route_egress(sink, 0, EGRESS_BROADCAST_NOTIFY, m.ptr(), m.size(), {}) == 0);
    CHECK(sink.log.empty());
}
