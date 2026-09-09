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
    bool send(uint32_t token, const uint8_t* d, uint32_t n, bool networkOnly) override {
        lastToken = token; lastNetworkOnly = networkOnly;
        log.push_back("send " + osc_test::parseAddress(d, n)); return true;
    }
    void broadcastNotify(const uint8_t* d, uint32_t n) override { log.push_back("notify " + osc_test::parseAddress(d, n)); }
    void broadcastLink(const uint8_t* d, uint32_t n) override { log.push_back("link " + osc_test::parseAddress(d, n)); }
    bool hasNotifySubscribers() const override { return true; }
    bool subscribeNotify(uint32_t) override { return true; }
    void subscribeNotifyPort(int) override {}
    void unsubscribeNotify(uint32_t) override {}
    void clearNotify() override {}
    bool subscribeLink(uint32_t) override { return true; }
    void unsubscribeLink(uint32_t) override {}
    void broadcastMidi(const uint8_t* d, uint32_t n) override { log.push_back("midi " + osc_test::parseAddress(d, n)); }
    bool subscribeMidi(uint32_t) override { return true; }
    void unsubscribeMidi(uint32_t) override {}
    void broadcastGamepad(const uint8_t* d, uint32_t n) override { log.push_back("gamepad " + osc_test::parseAddress(d, n)); }
    bool subscribeGamepad(uint32_t) override { return true; }
    void unsubscribeGamepad(uint32_t) override {}
    void broadcastOsc(const uint8_t* d, uint32_t n) override { log.push_back("osc " + osc_test::parseAddress(d, n)); }
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

    CHECK(route(7, EGRESS_REPLY));
    CHECK(sink.lastToken == 7);
    CHECK_FALSE(sink.lastNetworkOnly);
    CHECK(route(8, EGRESS_SEND_TO_CALLER));
    CHECK(sink.lastToken == 8);
    CHECK(sink.lastNetworkOnly);
    CHECK(route(0, EGRESS_BROADCAST_NOTIFY));
    CHECK(route(0, EGRESS_BROADCAST_LINK));
    CHECK(route(0, EGRESS_BROADCAST_MIDI));
    CHECK(route(0, EGRESS_BROADCAST_GAMEPAD));
    CHECK(route(0, EGRESS_BROADCAST_OSC));
    const std::vector<std::string> expect = { "send /x", "send /x", "notify /x", "link /x", "midi /x", "gamepad /x", "osc /x" };
    CHECK(sink.log == expect);
    CHECK(debug.empty());

    // A route nobody defined goes to the notify audience, as the engine's
    // gateway always sent it, rather than being lost.
    CHECK(route(0, 99));
    CHECK(sink.log.back() == "notify /x");
}

TEST_CASE("egress router: /clockwork/debug is a log line, not a message", "[egress][router]") {
    Sink sink;
    std::vector<std::string> debug;
    const auto m = osc_test::message("/clockwork/debug", "hello from the engine");
    CHECK(clockwork_route_egress(sink, 0, EGRESS_BROADCAST_NOTIFY, m.ptr(), m.size(),
                                 [&](const std::string& s) { debug.push_back(s); }));
    CHECK(sink.log.empty());
    REQUIRE(debug.size() == 1);
    CHECK(debug[0] == "hello from the engine");
    // Without a log to go to, it is dropped rather than sent as a message.
    CHECK_FALSE(clockwork_route_egress(sink, 0, EGRESS_BROADCAST_NOTIFY, m.ptr(), m.size(), {}));
    CHECK(sink.log.empty());
}
