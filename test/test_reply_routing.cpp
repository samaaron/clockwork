// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_reply_routing.cpp — the origin-token reply path.
 *
 * Replies/subscriptions are addressed by an origin token that is threaded from
 * the call ctx to each backend: OscEgress carries a per-call token through to
 * the transport for replies (dispatchEgress) and subscriptions.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/OscEgress.h"
#include "native/IOscTransport.h"
#include "shared_memory.h"   // EGRESS_REPLY, EGRESS_SEND_TO_CALLER

#include <string>
#include <vector>

// ── OscEgress: a per-call token reaches the transport ───────────────────────
namespace {
struct MockTransport : IOscTransport {
    uint32_t lastSendToken   = 0;
    bool     lastNetworkOnly = false;
    int      sendCount       = 0;
    uint32_t lastSubToken    = 0;

    bool send(uint32_t token, const uint8_t*, uint32_t, bool networkOnly) override {
        lastSendToken = token; lastNetworkOnly = networkOnly; ++sendCount; return true;
    }
    uint32_t broadcastNotify(const uint8_t*, uint32_t) override { return 0; }
    uint32_t broadcastLink(const uint8_t*, uint32_t) override { return 0; }
    bool hasNotifySubscribers() const override { return false; }
    bool subscribeNotify(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeNotify(uint32_t t) override { lastSubToken = t; }
    void clearNotify() override {}
    bool subscribeLink(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeLink(uint32_t t) override { lastSubToken = t; }
    uint32_t broadcastMidi(const uint8_t*, uint32_t) override { return 0; }
    bool subscribeMidi(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeMidi(uint32_t t) override { lastSubToken = t; }
    uint32_t broadcastGamepad(const uint8_t*, uint32_t) override { return 0; }
    bool subscribeGamepad(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeGamepad(uint32_t t) override { lastSubToken = t; }
    uint32_t broadcastOsc(const uint8_t*, uint32_t) override { return 0; }
    bool subscribeOsc(uint32_t t) override { lastSubToken = t; return true; }
    void unsubscribeOsc(uint32_t t) override { lastSubToken = t; }
};
}  // namespace

TEST_CASE("OscEgress routes a subscription to the per-call token", "[egress]") {
    MockTransport mock;
    OscEgress eg;
    eg.init(&mock, nullptr);

    CHECK(eg.subscribeCaller(0x1111));
    CHECK(mock.lastSubToken == 0x1111u);
    CHECK(eg.subscribeCallerToMidiNotify(0x2222));
    CHECK(mock.lastSubToken == 0x2222u);
    CHECK(eg.subscribeCallerToOscNotify(0x3333));
    CHECK(mock.lastSubToken == 0x3333u);
    CHECK(eg.subscribeCallerToLinkNotify(0x4444));
    CHECK(mock.lastSubToken == 0x4444u);
}

TEST_CASE("OscEgress delivers a reply to its origin token", "[egress]") {
    MockTransport mock;
    OscEgress eg;
    eg.init(&mock, nullptr);

    const uint8_t osc[] = {'/', 'x', 0, 0};
    // REPLY → send to the origin, in-process observers included.
    eg.dispatchEgress(0xABCD, EGRESS_REPLY, osc, sizeof(osc));
    CHECK(mock.lastSendToken == 0xABCDu);
    CHECK(mock.lastNetworkOnly == false);

    // SEND_TO_CALLER → same routing, network peers only.
    eg.dispatchEgress(0x7777, EGRESS_SEND_TO_CALLER, osc, sizeof(osc));
    CHECK(mock.lastSendToken == 0x7777u);
    CHECK(mock.lastNetworkOnly == true);

    // Two distinct origins get distinct replies — no shared/stale origin state.
    CHECK(mock.sendCount == 2);
}
