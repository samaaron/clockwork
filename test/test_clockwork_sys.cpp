// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_clockwork_sys.cpp — the one prefix clockwork reserves.
 *
 * src/clockwork_sys.h states the split mechanically: an address beginning
 * "/clockwork/" is clockwork's and is answered by clockwork; ANYTHING else
 * is forwarded to the DSP untouched. Since that rule is the boundary between
 * the two halves of this repository, it gets pinned from both sides:
 *
 *   - the pure predicate and the verb handler, called directly; and
 *   - the whole path, through the lanes ABI, so a client's bytes go in the IN
 *     ring and the reply comes back off the RT egress ring.
 *
 * The verbs are clockwork's own liveness surface. They name no DSP concept
 * and carry no definition format, which is exactly why they can be used here:
 * neither guest's vocabulary appears in this repository.
 */
#include "OscTestUtils.h"

#include "clockwork_sys.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

// Collect whatever handle_clockwork_sys_osc emits for one packet.
std::vector<osc_test::ParsedReply> answer(const osc_test::Packet& p) {
    std::vector<osc_test::ParsedReply> out;
    handle_clockwork_sys_osc(p.ptr(), p.size(), [&out](const uint8_t* d, uint32_t n) {
        out.push_back(osc_test::parseReply(d, n));
    });
    return out;
}

} // namespace

TEST_CASE("clockwork-sys: the prefix rule claims exactly clockwork's namespace", "[clockwork_sys]") {
    auto claims = [](const char* addr) {
        return clockwork_sys_claims(reinterpret_cast<const uint8_t*>(addr), std::strlen(addr));
    };

    REQUIRE(claims("/clockwork/ping"));
    REQUIRE(claims("/clockwork/anything/at/all"));

    // The trailing slash is the whole rule. Matching on "/clockwork" alone
    // would eat "/clockworks/..." and any longer name starting with those
    // letters, so the slash is what makes the claim exact.
    REQUIRE_FALSE(claims("/clockwork"));
    REQUIRE_FALSE(claims("/clockworks/ping"));
    REQUIRE_FALSE(claims("/clockwork-system-status"));
    REQUIRE_FALSE(claims("/clockwork-sys/ping"));
    // The names clockwork USED to answer on are now the DSP's like any other.
    REQUIRE_FALSE(claims("/engine/status"));
    REQUIRE_FALSE(claims("/clock/tempo/get"));
    REQUIRE_FALSE(claims("/dsp-probe/ping"));
    REQUIRE_FALSE(claims(""));

    // A bundle is a schedule, and the schedule belongs to the DSP.
    const char* bundle = "#bundle";
    REQUIRE_FALSE(clockwork_sys_claims(reinterpret_cast<const uint8_t*>(bundle), 7));

    REQUIRE_FALSE(clockwork_sys_claims(nullptr, 32));
}

TEST_CASE("clockwork-sys: ping answers pong", "[clockwork_sys]") {
    auto bare = answer(osc_test::message("/clockwork/ping"));
    REQUIRE(bare.size() == 1);
    REQUIRE(bare[0].address == "/clockwork/pong");
    REQUIRE(bare[0].argCount() == 0);

    // An id is echoed, so a client with several in flight can tell them apart.
    auto tagged = answer(osc_test::message("/clockwork/ping", 4242));
    REQUIRE(tagged.size() == 1);
    REQUIRE(tagged[0].address == "/clockwork/pong");
    REQUIRE(tagged[0].argCount() == 1);
    REQUIRE(tagged[0].argInt(0) == 4242);
}

TEST_CASE("clockwork-sys: sync answers synced with the caller's id, on the spot", "[clockwork_sys]") {
    // The barrier is clockwork's: answered at the point the message is
    // reached in the drain, which is after everything sent before it.
    auto r = answer(osc_test::message("/clockwork/sync", 77));
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].address == "/clockwork/synced");
    REQUIRE(r[0].argInt(0) == 77);
    // With no id, the reply still carries one a client can match on.
    auto bare = answer(osc_test::message("/clockwork/sync"));
    REQUIRE(bare[0].address == "/clockwork/synced");
    REQUIRE(bare[0].argInt(0) == 0);
}

TEST_CASE("clockwork-sys: echo returns what it was given", "[clockwork_sys]") {
    auto s = answer(osc_test::message("/clockwork/echo", "the pipe carries bytes"));
    REQUIRE(s.size() == 1);
    REQUIRE(s[0].address == "/clockwork/echo.reply");
    REQUIRE(s[0].argString(0) == "the pipe carries bytes");

    // A blob, because a string cannot carry a zero byte and a transport that
    // truncates at one would pass the string case.
    const std::vector<uint8_t> payload{0x00, 0x01, 0xff, 0x7f, 0x00, 0x80};
    auto b = answer(osc_test::messageWithBlob("/clockwork/echo",
                                              payload.data(), payload.size()));
    REQUIRE(b.size() == 1);
    REQUIRE(b[0].address == "/clockwork/echo.reply");
    REQUIRE(b[0].argBlob(0) == payload);
}

TEST_CASE("clockwork-sys: an unknown verb is refused, never forwarded", "[clockwork_sys]") {
    // The prefix is claimed whole. Falling through to the DSP would make the
    // rule depend on which verbs clockwork happens to implement today.
    auto r = answer(osc_test::message("/clockwork/no-such-verb"));
    REQUIRE(r.size() == 1);
    REQUIRE(r[0].address == "/clockwork/error");
    REQUIRE(r[0].argString(0) == "/clockwork/no-such-verb");
    REQUIRE_FALSE(r[0].argString(1).empty());

    // echo with the wrong argument type is refused rather than answered with
    // something invented.
    auto wrong = answer(osc_test::message("/clockwork/echo", 1, 2));
    REQUIRE(wrong.size() == 1);
    REQUIRE(wrong[0].address == "/clockwork/error");
}

TEST_CASE("clockwork-sys: an address outside the prefix is left alone", "[clockwork_sys]") {
    // handle_clockwork_sys_osc must not answer, and must SAY it did not, so the
    // caller forwards to the DSP.
    const auto p = osc_test::message("/dsp-probe/ping", 1);
    int emitted = 0;
    const bool claimed = handle_clockwork_sys_osc(p.ptr(), p.size(),
        [&emitted](const uint8_t*, uint32_t) { ++emitted; });
    REQUIRE_FALSE(claimed);
    REQUIRE(emitted == 0);
}

// NOTE: the end-to-end cases that pushed these verbs through the IN ring and
// read the replies off the RT egress ring are deliberately NOT here. They need
// a DSP attached to boot clockwork, and the boundary that defines a DSP is being
// redesigned. They also surfaced something worth fixing first, recorded here
// so it is not lost: driven through the lanes ABI alone, g_active_split used
// to be left unset, so OSC written to the IN ring was drained and dropped
// with "no backend for OSC". init_memory now publishes a default boundary by
// compare-exchange for exactly that case (audio_processor.cpp), so a bare
// lanes host does have a router; ClockworkEngine still overwrites it at boot.

// docs/SURFACE.md's line, as code: the client half is what a build may
// compile out (CLOCKWORK_CLIENT_VERBS), the transport half never. The
// predicate is consulted by the refusal path, so what it claims is exactly
// what a build without the client half refuses — and what it does not claim
// is exactly what such a build still answers.
TEST_CASE("clockwork-sys: the client half of the surface is one list", "[clockwork_sys][client_verbs]") {
    // Client: every send, the beat, the OSC forward.
    for (const char* v : { CLOCKWORK_SYS("midi/out/note_on"),   CLOCKWORK_SYS("midi/out/note_off"),
                           CLOCKWORK_SYS("midi/out/control_change"), CLOCKWORK_SYS("midi/out/pitch_bend"),
                           CLOCKWORK_SYS("midi/out/program_change"), CLOCKWORK_SYS("midi/out/channel_pressure"),
                           CLOCKWORK_SYS("midi/out/poly_pressure"), CLOCKWORK_SYS("midi/out/raw"),
                           CLOCKWORK_SYS("midi/out/sysex"),     CLOCKWORK_SYS("midi/out/clock"),
                           CLOCKWORK_SYS("midi/out/start"),     CLOCKWORK_SYS("midi/out/stop"),
                           CLOCKWORK_SYS("midi/out/continue"),  CLOCKWORK_SYS("midi/clock/beat"),
                           CLOCKWORK_SYS("osc/send") }) {
        INFO(v);
        CHECK(clockwork_sys_is_client_verb(v));
    }

    // Transport: owning a port, a device, the clock; being told about them.
    for (const char* v : { CLOCKWORK_SYS("midi/out/enable"),    CLOCKWORK_SYS("midi/in/enable"),
                           CLOCKWORK_SYS("midi/ports/list"),    CLOCKWORK_SYS("midi/notify/subscribe"),
                           CLOCKWORK_SYS("midi/clock/tick"),    CLOCKWORK_SYS("midi/clock/sync"),
                           CLOCKWORK_SYS("midi/clock/follow"),  CLOCKWORK_SYS("osc/cue-server/config"),
                           CLOCKWORK_SYS("osc/notify/subscribe"), CLOCKWORK_SYS("devices/list"),
                           CLOCKWORK_SYS("clock/tempo/get"),    CLOCKWORK_SYS("ping"),
                           // The timed store's verbs are the same split, made by CLOCKWORK_SCHEDULER.
                           CLOCKWORK_SYS("schedule"),           CLOCKWORK_SYS("sched/flush"),
                           "/dummy/ping" }) {
        INFO(v);
        CHECK_FALSE(clockwork_sys_is_client_verb(v));
    }
    CHECK_FALSE(clockwork_sys_is_client_verb(nullptr));
}
