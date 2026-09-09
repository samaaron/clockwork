// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_verbs.cpp — the client half of the verb surface, present.
 *
 * Compiled ONLY into a CLOCKWORK_CLIENT_VERBS=ON build (see test/CMakeLists.txt);
 * its twin, test_client_verbs_absent.cpp, only into an OFF one. Neither is
 * skipped at runtime, because a case that reports "passed" without running
 * proves nothing.
 *
 * docs/SURFACE.md: midi/out/…, midi/clock/beat and osc/send exist because a
 * client outside clockwork drives it one event at a time. With them built,
 * a send is a send — accepted, never refused — and reaches its endpoint at
 * once. (Timed sends are test_scheduled_out.cpp; the subsystem plumbing is
 * test_midi.cpp and test_osc.cpp. This file pins the surface's PRESENCE, the
 * exact thing its twin pins the absence of.)
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_prefix.h"

#include <chrono>
#include <string>

#if !CLOCKWORK_CLIENT_VERBS
#error "test_client_verbs.cpp belongs to a CLOCKWORK_CLIENT_VERBS=ON build only"
#endif

#ifdef CLOCKWORK_MIDI
TEST_CASE("client verbs: a MIDI send is accepted, not refused", "[client_verbs][midi]") {
    EngineFixture fx;

    osc_test::Builder noteOn;
    noteOn.begin(CLOCKWORK_SYS("midi/out/note_on"))
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100);
    fx.send(noteOn.end());

    osc_test::Builder beat;
    beat.begin(CLOCKWORK_SYS("midi/clock/beat")) << "*" << 500.0f;
    fx.send(beat.end());

    // No port is open, so nothing leaves — but nothing is refused either:
    // the verb is real in this build. (A refusal is /clockwork/error naming
    // the verb; see the twin.)
    OscReply err;
    CHECK_FALSE(fx.waitForReply(CLOCKWORK_SYS("error"), err, 400));

    // And the subsystem is still there behind it.
    fx.send(osc_test::message(CLOCKWORK_SYS("midi/ports/list")));
    OscReply r;
    CHECK(fx.waitForReply(CLOCKWORK_SYS("midi/ports.reply"), r));
}
#endif

#ifdef CLOCKWORK_OSC
TEST_CASE("client verbs: an immediate OSC send reaches its endpoint", "[client_verbs][osc]") {
    EngineFixture fx;

    juce::DatagramSocket listener;
    REQUIRE(listener.bindToPort(0, "127.0.0.1"));
    const int targetPort = listener.getBoundPort();

    const auto inner = osc_test::message("/now", static_cast<int32_t>(11));
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("osc/send"))
        << "127.0.0.1" << static_cast<osc::int32>(targetPort)
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    fx.send(b.end());

    // Not scheduled, so not held: the sink releases it at once.
    char buf[1024];
    juce::String ip;
    int fromPort = 0;
    bool got = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!got && std::chrono::steady_clock::now() < deadline) {
        if (listener.waitUntilReady(true, 50) == 1) {
            const int n = listener.read(buf, sizeof(buf), false, ip, fromPort);
            if (n > 0) {
                const auto p = osc_test::parseReply(reinterpret_cast<const uint8_t*>(buf),
                                                    static_cast<uint32_t>(n));
                CHECK(p.address == "/now");
                CHECK(p.argInt(0) == 11);
                got = true;
            }
        }
    }
    REQUIRE(got);

    OscReply err;
    CHECK_FALSE(fx.waitForReply(CLOCKWORK_SYS("error"), err, 200));
}
#endif
