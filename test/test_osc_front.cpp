// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_osc_front.cpp — the seam a product's front is installed at
 * (src/native/OscFront.h): a reply the front takes never reaches the
 * transport, a reply it declines does, and nothing else about the transport
 * changes for having a front in front of it.
 */
#include <catch2/catch_test_macros.hpp>

#include "OscFront.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

struct Inner final : IOscTransport {
    std::vector<std::pair<uint32_t, std::string>> sent;
    std::vector<std::string> log;
    bool send(uint32_t token, const uint8_t* d, uint32_t n, bool) override {
        sent.emplace_back(token, std::string(reinterpret_cast<const char*>(d), n));
        return true;
    }
    void broadcastNotify(const uint8_t*, uint32_t) override { log.push_back("notify"); }
    void broadcastLink(const uint8_t*, uint32_t) override { log.push_back("link"); }
    bool subscribers = true;
    bool hasNotifySubscribers() const override { return subscribers; }
    bool subscribeNotify(uint32_t) override { log.push_back("subNotify"); return true; }
    bool subscribeNotifyPort(int) override { log.push_back("subNotifyPort"); return true; }
    void unsubscribeNotify(uint32_t) override { log.push_back("unsubNotify"); }
    void clearNotify() override { log.push_back("clearNotify"); }
    bool subscribeLink(uint32_t) override { log.push_back("subLink"); return true; }
    void unsubscribeLink(uint32_t) override { log.push_back("unsubLink"); }
    void broadcastMidi(const uint8_t*, uint32_t) override { log.push_back("midi"); }
    bool subscribeMidi(uint32_t) override { log.push_back("subMidi"); return true; }
    void unsubscribeMidi(uint32_t) override { log.push_back("unsubMidi"); }
    void broadcastGamepad(const uint8_t*, uint32_t) override { log.push_back("gamepad"); }
    bool subscribeGamepad(uint32_t) override { log.push_back("subGamepad"); return true; }
    void unsubscribeGamepad(uint32_t) override { log.push_back("unsubGamepad"); }
    void broadcastOsc(const uint8_t*, uint32_t) override { log.push_back("osc"); }
    bool subscribeOsc(uint32_t) override { log.push_back("subOsc"); return true; }
    void unsubscribeOsc(uint32_t) override { log.push_back("unsubOsc"); }
};

// Takes every reply whose bytes start with "/mine"; counts the pushes it hears.
struct Front final : OscFront {
    int seen = 0;
    std::vector<std::string> noticed;
    bool ingress(const uint8_t*, uint32_t, uint32_t) override { return false; }
    bool egress(uint32_t, const uint8_t* d, uint32_t n) override {
        ++seen;
        return n >= 5 && std::memcmp(d, "/mine", 5) == 0;
    }
    void notice(const uint8_t* d, uint32_t n) override {
        noticed.emplace_back(reinterpret_cast<const char*>(d), n);
    }
    const char* describe() const override { return "test front"; }
};

} // namespace

TEST_CASE("fronted transport: a taken reply stops at the front, a declined one goes on", "[front]") {
    Inner inner;
    Front front;
    FrontedTransport t;
    t.attach(&inner, &front);

    const char* mine = "/mine\0\0\0,\0\0\0";
    const char* other = "/other\0\0,\0\0\0";
    CHECK(t.send(7, reinterpret_cast<const uint8_t*>(mine), 12, false));
    CHECK(t.send(7, reinterpret_cast<const uint8_t*>(other), 12, false));
    CHECK(front.seen == 2);
    REQUIRE(inner.sent.size() == 1);
    CHECK(inner.sent[0].first == 7);
    CHECK(inner.sent[0].second.rfind("/other", 0) == 0);

    // Detached, everything goes on.
    t.detachFront();
    CHECK(t.send(7, reinterpret_cast<const uint8_t*>(mine), 12, false));
    CHECK(inner.sent.size() == 2);
    CHECK(front.seen == 2);
}

TEST_CASE("fronted transport: every other method is the inner transport's", "[front]") {
    Inner inner;
    Front front;
    FrontedTransport t;
    t.attach(&inner, &front);
    const uint8_t d[4] = {};
    t.broadcastNotify(d, 4); t.broadcastLink(d, 4);
    CHECK(t.hasNotifySubscribers());
    CHECK(t.subscribeNotify(1)); CHECK(t.subscribeNotifyPort(2)); t.unsubscribeNotify(1); t.clearNotify();
    CHECK(t.subscribeLink(1)); t.unsubscribeLink(1);
    t.broadcastMidi(d, 4); CHECK(t.subscribeMidi(1)); t.unsubscribeMidi(1);
    t.broadcastGamepad(d, 4); CHECK(t.subscribeGamepad(1)); t.unsubscribeGamepad(1);
    t.broadcastOsc(d, 4); CHECK(t.subscribeOsc(1)); t.unsubscribeOsc(1);
    const std::vector<std::string> expect = {
        "notify", "link", "subNotify", "subNotifyPort", "unsubNotify", "clearNotify",
        "subLink", "unsubLink", "midi", "subMidi", "unsubMidi",
        "gamepad", "subGamepad", "unsubGamepad", "osc", "subOsc", "unsubOsc" };
    CHECK(inner.log == expect);

    // With nothing attached, nothing happens and nothing is claimed.
    FrontedTransport bare;
    CHECK_FALSE(bare.send(1, d, 4, false));
    CHECK_FALSE(bare.hasNotifySubscribers());
    CHECK_FALSE(bare.subscribeNotify(1));
}

TEST_CASE("fronted transport: a push to the notify targets is heard by the front and still goes to them",
          "[front]") {
    // A front that stages state in the engine (samples in a lane) has to
    // learn when a rebuild dropped it all: /clockwork/setup goes to every
    // notify target, and the front hears it on the way. Heard, never taken:
    // the targets get it whether or not a front is attached, and a front
    // hears only the notify pushes, not Link's, MIDI's or the gamepads'.
    Inner inner;
    Front front;
    FrontedTransport t;
    t.attach(&inner, &front);
    const char setup[] = "/clockwork/setup";
    t.broadcastNotify(reinterpret_cast<const uint8_t*>(setup), sizeof setup);
    CHECK(front.noticed == std::vector<std::string>{std::string(setup, sizeof setup)});
    CHECK(inner.log == std::vector<std::string>{"notify"});

    const uint8_t d[4] = {};
    t.broadcastLink(d, 4); t.broadcastMidi(d, 4); t.broadcastGamepad(d, 4); t.broadcastOsc(d, 4);
    CHECK(front.noticed.size() == 1);

    t.detachFront();
    t.broadcastNotify(d, 4);
    CHECK(front.noticed.size() == 1);
    CHECK(inner.log.back() == "notify");
}

TEST_CASE("fronted transport: an attached front is a notify target, so the engine builds the pushes it hears",
          "[front]") {
    // The engine builds a push only for an audience (OscEgress::sendStateChange
    // asks hasNotifySubscribers first; a rebuild's setup is the one written
    // whoever listens). A host whose clients never subscribe — an application
    // with the engine linked in — still has a front that hears what the engine
    // pushes, so the front counts, whatever the inner transport says; without
    // a front the answer is the inner transport's.
    Inner inner;
    inner.subscribers = false;
    Front front;
    FrontedTransport t;
    t.attach(&inner, &front);
    CHECK(t.hasNotifySubscribers());
    t.detachFront();
    CHECK_FALSE(t.hasNotifySubscribers());
    inner.subscribers = true;
    CHECK(t.hasNotifySubscribers());
}
