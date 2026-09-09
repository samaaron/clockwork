// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_inbound_events.cpp — an inbound event is one thing, wherever it came
 * from.
 *
 * What a MIDI port or a game controller sent enters the engine through the
 * IN ring — written there by a native subsystem's callback, or by a host's
 * front on the web's main thread — and is answered on the audio thread by
 * one route (clockwork_event_route, clockwork_sys.h): out over the egress to
 * the subsystem's audience, and to the guest if it asked for events
 * (DspInfo::wants_events). Driven through the lanes ABI, which is the
 * worklet's shape and the native engine's underneath. What these pin:
 *
 *   - an event goes out on the audience route for its subsystem, from
 *     origin 0, byte for byte — a trailing arrival timetag included;
 *   - the guest that asked hears it, on the audio thread, through dsp_osc;
 *   - the ports and devices pushes are events too;
 *   - a VERB that lives under an event's name — midi/in/enable,
 *     midi/ports/list — is not an event: it takes the verb's path.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "clockwork_prefix.h"
#include "lanes/lanes.h"
#include "shared_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kClient = 0x3e17;

const lanes_test::EgressFrame* find(const std::vector<lanes_test::EgressFrame>& frames,
                                    const char* address) {
    for (const auto& f : frames)
        if (osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    return nullptr;
}

// What the guest heard: "/dummy/events ,si <last address> <count>".
struct Heard { std::string last; int32_t count = -1; };
Heard askGuest() {
    lanes_test::drainRt();
    const auto q = osc_test::message("/dummy/events/get");
    REQUIRE(lanes_test::ingress(q.ptr(), q.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, "/dummy/events");
    const auto* f = find(frames, "/dummy/events");
    REQUIRE(f != nullptr);
    const auto r = osc_test::parseReply(f->data.data(), static_cast<uint32_t>(f->data.size()));
    return { r.argString(0), r.argInt(1) };
}

} // namespace

TEST_CASE("inbound events: a MIDI event goes to the MIDI audience and to the guest, whole",
          "[events][lanes]") {
    lanes_test::boot();
    const Heard before = askGuest();
    lanes_test::drainRt();

    // As the web front (or the native callback) writes it: with the moment
    // it arrived as a trailing timetag.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/in/note_on"))
        << "keys" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100) << osc::TimeTag(0xdeadbeef00000001ull);
    const auto ev = b.end();
    REQUIRE(lanes_test::ingress(ev.ptr(), ev.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("midi/in/note_on"));
    const auto* out = find(frames, CLOCKWORK_SYS("midi/in/note_on"));
    REQUIRE(out != nullptr);
    CHECK(out->route == EGRESS_BROADCAST_MIDI);
    CHECK(out->sourceId == 0);                 // an event answers nobody
    CHECK(out->data == ev.data);               // byte for byte, timetag included
    CHECK(find(frames, CLOCKWORK_SYS("error")) == nullptr);

    const Heard after = askGuest();
    CHECK(after.count == before.count + 1);
    CHECK(after.last == CLOCKWORK_SYS("midi/in/note_on"));
}

TEST_CASE("inbound events: a gamepad event goes to the gamepad audience", "[events][lanes]") {
    lanes_test::boot();
    lanes_test::drainRt();
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("gamepad/in/button")) << "pad" << "a" << static_cast<osc::int32>(1) << 1.0f;
    const auto ev = b.end();
    REQUIRE(lanes_test::ingress(ev.ptr(), ev.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("gamepad/in/button"));
    const auto* out = find(frames, CLOCKWORK_SYS("gamepad/in/button"));
    REQUIRE(out != nullptr);
    CHECK(out->route == EGRESS_BROADCAST_GAMEPAD);
    CHECK(out->data == ev.data);
    CHECK(askGuest().last == CLOCKWORK_SYS("gamepad/in/button"));
}

TEST_CASE("inbound events: the ports and devices pushes are events too", "[events][lanes]") {
    lanes_test::boot();
    lanes_test::drainRt();
    osc_test::Builder p;
    p.begin(CLOCKWORK_SYS("midi/ports")) << static_cast<osc::int32>(1) << "keys" << static_cast<osc::int32>(1)
                                         << static_cast<osc::int32>(0);
    const auto ports = p.end();
    REQUIRE(lanes_test::ingress(ports.ptr(), ports.size(), kClient));
    auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("midi/ports"));
    const auto* out = find(frames, CLOCKWORK_SYS("midi/ports"));
    REQUIRE(out != nullptr);
    CHECK(out->route == EGRESS_BROADCAST_MIDI);

    osc_test::Builder d;
    d.begin(CLOCKWORK_SYS("gamepad/devices")) << static_cast<osc::int32>(0);
    const auto devices = d.end();
    REQUIRE(lanes_test::ingress(devices.ptr(), devices.size(), kClient));
    frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("gamepad/devices"));
    const auto* dev = find(frames, CLOCKWORK_SYS("gamepad/devices"));
    REQUIRE(dev != nullptr);
    CHECK(dev->route == EGRESS_BROADCAST_GAMEPAD);
}

TEST_CASE("inbound events: a verb under an event's name is still a verb", "[events][lanes]") {
    // midi/in/enable and midi/ports/list live under midi/in/ and midi/ports.
    // Longest match wins, and they are registered as their own routes: on a
    // bare host they are refused as unknown (the host's end of the chain),
    // never broadcast as events, and never handed to the guest.
    lanes_test::boot();
    clockwork_host_forward(0);
    const Heard before = askGuest();
    lanes_test::drainRt();

    osc_test::Builder e;
    e.begin(CLOCKWORK_SYS("midi/in/enable")) << "keys" << static_cast<osc::int32>(1);
    const auto enable = e.end();
    REQUIRE(lanes_test::ingress(enable.ptr(), enable.size(), kClient));
    auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    REQUIRE(find(frames, CLOCKWORK_SYS("error")) != nullptr);
    CHECK(find(frames, CLOCKWORK_SYS("midi/in/enable")) == nullptr);

    const auto list = osc_test::message(CLOCKWORK_SYS("midi/ports/list"));
    REQUIRE(lanes_test::ingress(list.ptr(), list.size(), kClient));
    frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    REQUIRE(find(frames, CLOCKWORK_SYS("error")) != nullptr);
    CHECK(find(frames, CLOCKWORK_SYS("midi/ports/list")) == nullptr);

    CHECK(askGuest().count == before.count);
}
