// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_token.cpp — a message's token, out to the host.
 *
 * Each message has a slot beside it for a number of the sender's choosing.
 * Clockwork carries it to the guest and never acts on it. Clockwork's own
 * answers carry the token of the message they answer; a guest's answers
 * carry what the guest puts there (the dummy guest passes the token on); and
 * a notification carries 0. Whatever is in the slot is delivered with the
 * message.
 *
 * The token inside the engine (the rings, a held message, a refusal, the
 * guest's side) is pinned by test_dsp_boundary.cpp, test_lanes.cpp and
 * test_scheduler.cpp. These cases pin it at the host's delivery.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_prefix.h"

TEST_CASE("token: a guest's answer is delivered with the token its message was sent with", "[token]") {
    EngineFixture fx;
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(fx.engine().ingest(ping.ptr(), ping.size(), 7) == CLOCKWORK_OK);
    OscReply pong;
    REQUIRE(fx.waitForReply("/dummy/pong", pong));
    CHECK(pong.token == 7);
}

TEST_CASE("token: clockwork's own answer is delivered with the token its message was sent with", "[token]") {
    EngineFixture fx;
    const auto get = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"));
    REQUIRE(fx.engine().ingest(get.ptr(), get.size(), 9) == CLOCKWORK_OK);
    OscReply tempo;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), tempo));
    CHECK(tempo.token == 9);
}

TEST_CASE("token: a message sent without a token is answered without one", "[token]") {
    EngineFixture fx;
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(fx.engine().ingest(ping.ptr(), ping.size(), 0) == CLOCKWORK_OK);
    OscReply pong;
    REQUIRE(fx.waitForReply("/dummy/pong", pong));
    CHECK(pong.token == 0);
}

TEST_CASE("token: two messages sent with different tokens are each answered with their own", "[token]") {
    EngineFixture fx;
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(fx.engine().ingest(ping.ptr(), ping.size(), 100) == CLOCKWORK_OK);
    REQUIRE(fx.engine().ingest(ping.ptr(), ping.size(), 200) == CLOCKWORK_OK);
    REQUIRE(fx.pollUntil([&] {
        int n = 0;
        for (const auto& r : fx.allReplies()) if (r.address == "/dummy/pong") ++n;
        return n == 2;
    }));
    std::vector<uint32_t> tokens;
    for (const auto& r : fx.allReplies()) if (r.address == "/dummy/pong") tokens.push_back(r.token);
    CHECK(tokens == std::vector<uint32_t>{ 100, 200 });
}

// Game controller input is where a notice is easiest to cause; a build
// without it has no such notice to deliver.
#ifdef CLOCKWORK_GAMEPAD
TEST_CASE("token: a notification answers no message, and is delivered with no token", "[token]") {
    EngineFixture fx;
    // The client asks to be told about controllers, then asks for a refresh.
    // Both asks carry tokens; the notice the refresh causes is a broadcast,
    // and carries none.
    const auto subscribe = osc_test::message(CLOCKWORK_SYS("gamepad/notify/subscribe"));
    REQUIRE(fx.engine().ingest(subscribe.ptr(), subscribe.size(), 5) == CLOCKWORK_OK);
    OscReply snapshot;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("gamepad/devices.reply"), snapshot));
    CHECK(snapshot.token == 5);

    const auto refresh = osc_test::message(CLOCKWORK_SYS("gamepad/refresh"));
    REQUIRE(fx.engine().ingest(refresh.ptr(), refresh.size(), 6) == CLOCKWORK_OK);
    OscReply notice;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("gamepad/devices"), notice));
    CHECK(notice.token == 0);
}
#endif
