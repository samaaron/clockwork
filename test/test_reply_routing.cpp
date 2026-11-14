// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_reply_routing.cpp — the engine's gateway hands every egress frame to
 * the transport (OscEgress::dispatchEgress): there is one client.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/OscEgress.h"
#include "native/IOscTransport.h"

#include <vector>

namespace {
struct MockTransport : IOscTransport {
    int delivered = 0;
    std::vector<uint32_t> tokens;
    bool deliver(const uint8_t*, uint32_t, uint32_t token) override { ++delivered; tokens.push_back(token); return true; }
};
}  // namespace

TEST_CASE("OscEgress hands every frame to the transport", "[egress]") {
    MockTransport mock;
    OscEgress eg;
    eg.init(&mock, nullptr);

    const uint8_t osc[] = {'/', 'x', 0, 0};
    eg.dispatchEgress(0, osc, sizeof(osc));
    eg.dispatchEgress(42, osc, sizeof(osc));
    CHECK(mock.delivered == 2);
    // Each with the token its frame carried: 0 for a notification, or the
    // token of the message it answers.
    CHECK(mock.tokens == std::vector<uint32_t>{ 0, 42 });
}
