// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_verbs_absent.cpp — what clockwork does with a client verb when
 * it has none. Compiled ONLY into a CLOCKWORK_CLIENT_VERBS=OFF build (see
 * test/CMakeLists.txt); its twin, test_client_verbs.cpp, only into an ON one.
 * Neither is skipped at runtime, because a case that reports "passed" without
 * running proves nothing.
 *
 * docs/SURFACE.md: the option removes the client half — midi/out/… (but not
 * midi/out/enable, which is the transport's), midi/clock/beat, osc/send —
 * for a guest that drives its own output through clockwork_sink_send. What
 * this file pins:
 *
 *   - each of them is REFUSED, and refused BY NAME: /clockwork/error carries
 *     the address and a reason naming the option. A client learns this build
 *     dropped the verb, not that the verb was never real — which is what an
 *     "unknown verb" refusal, or silence, would have said.
 *
 *   - the refusal reaches the caller whichever way the verb arrived: sent
 *     now, or fired by the scheduler out of a /clockwork/schedule.
 *
 *   - the transport half is untouched. Ports are listed, a port is enabled,
 *     a subscription is acked. The option is about who drives, not about
 *     owning the hardware.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clock/clock_math.h"   // wallClockNTP
#include "clockwork_prefix.h"
#include "clockwork_sys.h"      // CLOCKWORK_CLIENT_VERBS_ABSENT

#include <string>

#if CLOCKWORK_CLIENT_VERBS
#error "test_client_verbs_absent.cpp belongs to a CLOCKWORK_CLIENT_VERBS=OFF build only"
#endif

namespace {

// Sends `pkt` and requires a refusal naming `addr` with the option's reason.
void requireRefused(EngineFixture& fx, const osc_test::Packet& pkt, const char* addr) {
    fx.clearReplies();
    fx.send(pkt);
    OscReply err;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("error"), err));
    const auto p = err.parsed();
    REQUIRE(p.argCount() >= 2);
    CHECK(p.argString(0) == addr);
    CHECK(p.argString(1) == CLOCKWORK_CLIENT_VERBS_ABSENT);
}

osc_test::Packet noteOn() {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/out/note_on"))
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100);
    return b.end();
}

} // namespace

#ifdef CLOCKWORK_MIDI
TEST_CASE("no client verbs: every MIDI send is refused by name", "[client_verbs][midi]") {
    EngineFixture fx;

    requireRefused(fx, noteOn(), CLOCKWORK_SYS("midi/out/note_on"));

    osc_test::Builder raw;
    raw.begin(CLOCKWORK_SYS("midi/out/raw")) << "*" << osc::Blob("\x90\x3c\x64", 3);
    requireRefused(fx, raw.end(), CLOCKWORK_SYS("midi/out/raw"));

    osc_test::Builder start;
    start.begin(CLOCKWORK_SYS("midi/out/start")) << "*";
    requireRefused(fx, start.end(), CLOCKWORK_SYS("midi/out/start"));

    osc_test::Builder beat;
    beat.begin(CLOCKWORK_SYS("midi/clock/beat")) << "*" << 500.0f;
    requireRefused(fx, beat.end(), CLOCKWORK_SYS("midi/clock/beat"));
}

TEST_CASE("no client verbs: the MIDI transport half still answers", "[client_verbs][midi]") {
    EngineFixture fx;

    // Owning a port is the transport's: enabling one is not a client verb,
    // whatever its address looks like. No refusal.
    osc_test::Builder enable;
    enable.begin(CLOCKWORK_SYS("midi/out/enable")) << "no-such-port" << static_cast<osc::int32>(1);
    fx.send(enable.end());
    OscReply err;
    CHECK_FALSE(fx.waitForReply(CLOCKWORK_SYS("error"), err, 400));

    fx.send(osc_test::message(CLOCKWORK_SYS("midi/ports/list")));
    OscReply r;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("midi/ports.reply"), r));

    fx.send(osc_test::message(CLOCKWORK_SYS("midi/notify/subscribe"), int32_t{41}));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("midi/notify/subscribe.reply"), r));
    CHECK(lastInt(r) == 41);
}

#if CLOCKWORK_SCHEDULER
TEST_CASE("no client verbs: a scheduled send is refused when it fires", "[client_verbs][midi][schedule]") {
    EngineFixture fx;

    // The wire form a client actually uses: the verb inside a timetagged
    // /clockwork/schedule. The scheduler fires it and the refusal names the
    // INNER verb, since that is what the client asked for.
    const auto inner = noteOn();
    osc_test::Builder at;
    at.begin(CLOCKWORK_SYS("schedule"))
        << (wallClockNTP() - 1.0)   // just past: due at once
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    requireRefused(fx, at.end(), CLOCKWORK_SYS("midi/out/note_on"));
}
#endif
#endif // CLOCKWORK_MIDI

#ifdef CLOCKWORK_OSC
TEST_CASE("no client verbs: osc/send is refused by name", "[client_verbs][osc]") {
    EngineFixture fx;

    const auto inner = osc_test::message("/hello", static_cast<int32_t>(1));
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("osc/send"))
        << "127.0.0.1" << static_cast<osc::int32>(9)
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    requireRefused(fx, b.end(), CLOCKWORK_SYS("osc/send"));
}

TEST_CASE("no client verbs: the OSC transport half still answers", "[client_verbs][osc]") {
    EngineFixture fx;

    fx.send(osc_test::message(CLOCKWORK_SYS("osc/notify/subscribe"), int32_t{42}));
    OscReply r;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("osc/notify/subscribe.reply"), r));
    CHECK(lastInt(r) == 42);

    osc_test::Builder cfg;
    cfg.begin(CLOCKWORK_SYS("osc/cue-server/cues-on")) << static_cast<osc::int32>(0);
    fx.send(cfg.end());
    OscReply err;
    CHECK_FALSE(fx.waitForReply(CLOCKWORK_SYS("error"), err, 300));
}
#endif // CLOCKWORK_OSC
