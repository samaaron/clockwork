// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_verb_refusals.cpp — every verb a subsystem claims is answered.
 *
 * clockwork claims /clockwork/ whole, so nothing under it reaches the DSP, and
 * a client that sends a verb it got wrong must hear so: "/clockwork/error
 * <address> <reason>", with the reasons the web's host front gives
 * (js/lib/host_front.js), so a client reads the same refusal on every host.
 * A typo, or a verb from a newer client, is "unknown clockwork verb"; a verb
 * that exists but whose arguments it cannot read is "malformed".
 *
 * The subsystems' namespaces (midi/, gamepad/, osc/) once answered neither:
 * the Rust decoders returned nothing for both, and the verb was dropped.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_prefix.h"

#include <string>

namespace {

constexpr const char* kUnknown   = "unknown clockwork verb";
constexpr const char* kMalformed = "malformed";

void requireRefused(EngineFixture& fx, const osc_test::Packet& pkt,
                    const std::string& address, const std::string& reason) {
    fx.clearReplies();
    fx.send(pkt);
    OscReply err;
    INFO("refusal of " << address);
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("error"), err));
    const auto p = err.parsed();
    REQUIRE(p.argCount() == 2);
    CHECK(p.argString(0) == address);
    CHECK(p.argString(1) == reason);
}

}  // namespace

#ifdef CLOCKWORK_MIDI
TEST_CASE("an unknown MIDI verb is refused", "[refusals][midi]") {
    EngineFixture fx;
    requireRefused(fx, osc_test::message("/clockwork/midi/bogus"),
                   "/clockwork/midi/bogus", kUnknown);
    requireRefused(fx, osc_test::message("/clockwork/midi/clock/bogus"),
                   "/clockwork/midi/clock/bogus", kUnknown);
}

TEST_CASE("a MIDI verb it cannot read is refused as malformed", "[refusals][midi]") {
    EngineFixture fx;
    // in/enable takes a port and a flag.
    requireRefused(fx, osc_test::message("/clockwork/midi/in/enable"),
                   "/clockwork/midi/in/enable", kMalformed);
    requireRefused(fx, osc_test::message("/clockwork/midi/out/enable", "a-port"),
                   "/clockwork/midi/out/enable", kMalformed);
#if CLOCKWORK_CLIENT_VERBS
    // A send whose channel, note and velocity are missing.
    requireRefused(fx, osc_test::message("/clockwork/midi/out/note_on", "*"),
                   "/clockwork/midi/out/note_on", kMalformed);
#endif
}
#endif // CLOCKWORK_MIDI

#ifdef CLOCKWORK_GAMEPAD
TEST_CASE("an unknown gamepad verb is refused", "[refusals][gamepad]") {
    EngineFixture fx;
    requireRefused(fx, osc_test::message("/clockwork/gamepad/bogus"),
                   "/clockwork/gamepad/bogus", kUnknown);
}

TEST_CASE("a gamepad verb it cannot read is refused as malformed", "[refusals][gamepad]") {
    EngineFixture fx;
    // enable takes a pad and a flag; rumble a pad, two strengths and a duration.
    requireRefused(fx, osc_test::message("/clockwork/gamepad/enable"),
                   "/clockwork/gamepad/enable", kMalformed);
    requireRefused(fx, osc_test::message("/clockwork/gamepad/out/rumble", "*"),
                   "/clockwork/gamepad/out/rumble", kMalformed);
}
#endif // CLOCKWORK_GAMEPAD

#ifdef CLOCKWORK_OSC
TEST_CASE("an unknown OSC verb is refused", "[refusals][osc]") {
    EngineFixture fx;
    requireRefused(fx, osc_test::message("/clockwork/osc/bogus"),
                   "/clockwork/osc/bogus", kUnknown);
}

#if CLOCKWORK_CLIENT_VERBS
TEST_CASE("an osc/send it cannot read is refused as malformed", "[refusals][osc]") {
    EngineFixture fx;
    // osc/send takes a host, a port and the message to send as a blob.
    requireRefused(fx, osc_test::message("/clockwork/osc/send"),
                   "/clockwork/osc/send", kMalformed);
    osc_test::Builder noBlob;
    noBlob.begin("/clockwork/osc/send") << "127.0.0.1" << static_cast<osc::int32>(9000);
    requireRefused(fx, noBlob.end(), "/clockwork/osc/send", kMalformed);
}
#endif
#endif // CLOCKWORK_OSC
