// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_osc_front.cpp — the seam a product's front is installed at
 * (src/comms/OscFront.h): a frame the front takes never reaches the
 * transport, and one it declines does.
 */
#include <catch2/catch_test_macros.hpp>

#include "OscFront.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Inner final : IOscTransport {
    std::vector<std::string> sent;
    bool delivers = true;
    ClockworkCore* core = nullptr;
    void bindCore(ClockworkCore* c) override { core = c; }
    bool deliver(const uint8_t* d, uint32_t n) override {
        sent.emplace_back(reinterpret_cast<const char*>(d), n);
        return delivers;
    }
};

// Takes every frame whose bytes start with "/mine".
struct Front final : OscFront {
    int seen = 0;
    bool ingress(const uint8_t*, uint32_t, uint32_t) override { return false; }
    bool egress(const uint8_t* d, uint32_t n) override {
        ++seen;
        return n >= 5 && std::memcmp(d, "/mine", 5) == 0;
    }
    const char* describe() const override { return "test front"; }
};

} // namespace

TEST_CASE("fronted transport: a taken frame stops at the front, a declined one goes on", "[front]") {
    Inner inner;
    Front front;
    FrontedTransport t;
    t.attach(&inner, &front);

    const char* mine = "/mine\0\0\0,\0\0\0";
    const char* other = "/other\0\0,\0\0\0";
    CHECK(t.deliver(reinterpret_cast<const uint8_t*>(mine), 12));
    CHECK(t.deliver(reinterpret_cast<const uint8_t*>(other), 12));
    CHECK(front.seen == 2);
    REQUIRE(inner.sent.size() == 1);
    CHECK(inner.sent[0].rfind("/other", 0) == 0);

    // What the inner transport could not deliver is said.
    inner.delivers = false;
    CHECK_FALSE(t.deliver(reinterpret_cast<const uint8_t*>(other), 12));
    inner.delivers = true;

    // Detached, everything goes on.
    t.detachFront();
    CHECK(t.deliver(reinterpret_cast<const uint8_t*>(mine), 12));
    CHECK(inner.sent.size() == 3);
    CHECK(front.seen == 3);
}

// The engine tells its transport its core at boot and takes it back at
// shutdown; the transport behind a front is the one that uses it.
TEST_CASE("fronted transport: the engine's core is the inner transport's to know", "[front]") {
    Inner inner;
    Front front;
    FrontedTransport t;
    t.attach(&inner, &front);
    auto* const core = reinterpret_cast<ClockworkCore*>(&inner);   // an address, never followed
    t.bindCore(core);
    CHECK(inner.core == core);
    t.bindCore(nullptr);
    CHECK(inner.core == nullptr);
}

TEST_CASE("fronted transport: with nothing attached a frame reaches no one", "[front]") {
    FrontedTransport bare;
    const uint8_t d[4] = {};
    CHECK_FALSE(bare.deliver(d, 4));
}
