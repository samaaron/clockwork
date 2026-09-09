// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_egress.cpp — the client reads the engine's egress itself.
 *
 * The engine writes two rings: the audio thread's replies (OUT) and the
 * NRT gateway's (NRT-out — the subsystems, the state changes, the
 * broadcasts). One read function, clockwork_client_poll, takes from both,
 * each frame with the origin it answers and the route it was sent by. A
 * host that drains egress itself (Config::hostDrainsEgress) tells the engine
 * so, and the engine's own gateway then leaves the rings alone: the host is
 * the consumer, on native as the JavaScript client already is on the web.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "shared_memory.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Frame { uint32_t origin, route; std::string address; };

// Everything the client can take right now.
std::vector<Frame> takeAll(ClockworkClient* c) {
    std::vector<Frame> out;
    ClockworkClientMessage batch[64];
    for (;;) {
        const uint32_t n = clockwork_client_poll(c, batch, 64);
        for (uint32_t i = 0; i < n; ++i)
            out.push_back({ batch[i].origin, batch[i].route, osc_test::parseAddress(batch[i].bytes, batch[i].length) });
        if (n < 64) break;
    }
    return out;
}

// Polls until a frame at `address` arrives, or gives up.
bool waitForFrame(ClockworkClient* c, const char* address, Frame* out, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const Frame& f : takeAll(c))
            if (f.address == address) { if (out) *out = f; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("client egress: one poll takes from both rings, each frame with its origin and route",
          "[client][egress]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    ClockworkClient* c = fx.engine().egressClient();
    REQUIRE(c != nullptr);

    // Boot wrote to the NRT ring before anyone polled: the state changes are
    // still there, routed to the notify audience, and the client takes them.
    bool sawNotify = false;
    for (const Frame& f : takeAll(c)) if (f.route == EGRESS_BROADCAST_NOTIFY) sawNotify = true;
    CHECK(sawNotify);

    // The audio thread answers on the OUT ring, to the token that asked.
    const auto ping = osc_test::message("/dummy/ping");
    fx.engine().ingest(ping.ptr(), ping.size(), 7);
    Frame pong;
    REQUIRE(waitForFrame(c, "/dummy/pong", &pong));
    CHECK(pong.origin == 7);
    CHECK(pong.route == EGRESS_REPLY);

    // The NRT gateway answers on the NRT-out ring, to its token, and the
    // same poll takes that too.
    const auto notify = osc_test::message("/clockwork/notify", int32_t{1});
    fx.engine().ingest(notify.ptr(), notify.size(), 9);
    Frame reply;
    REQUIRE(waitForFrame(c, "/clockwork/notify.reply", &reply));
    CHECK(reply.origin == 9);
    CHECK(reply.route == EGRESS_REPLY);
}

TEST_CASE("client egress: with the host draining, the engine's gateway leaves the rings alone",
          "[client][egress]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    // The fixture listens through the engine's transport, which the gateway
    // used to feed. Nothing reaches it now: the frames wait in the rings for
    // the host.
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    CHECK_FALSE(fx.waitForReply("/dummy/pong", r, 400));
    Frame pong;
    CHECK(waitForFrame(fx.engine().egressClient(), "/dummy/pong", &pong));
}

TEST_CASE("client egress: without it, the engine delivers as it always did", "[client][egress]") {
    EngineFixture fx;   // hostDrainsEgress false
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    CHECK(fx.waitForReply("/dummy/pong", r));
}
