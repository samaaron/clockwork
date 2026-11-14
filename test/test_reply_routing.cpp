// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_reply_routing.cpp — the engine's gateway hands every egress frame to
 * the transport (OscEgress::dispatchEgress): there is one client.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/OscEgress.h"
#include "native/IOscTransport.h"

namespace {
struct MockTransport : IOscTransport {
    int delivered = 0;
    bool deliver(const uint8_t*, uint32_t) override { ++delivered; return true; }
};
}  // namespace

TEST_CASE("OscEgress hands every frame to the transport", "[egress]") {
    MockTransport mock;
    OscEgress eg;
    eg.init(&mock, nullptr);

    const uint8_t osc[] = {'/', 'x', 0, 0};
    eg.dispatchEgress(osc, sizeof(osc));
    eg.dispatchEgress(osc, sizeof(osc));
    CHECK(mock.delivered == 2);
}
