// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_osc_ingress.cpp — pins clockwork's two routing contracts:
 *
 *   src/OscIngress.h — the single, modular, capability-gated routing primitive
 *   every build funnels through (native UDP, NIF, worklet channel, SLIP). A
 *   registered address prefix peels off to its handler, longest prefix wins,
 *   and EVERYTHING else — including every bundle — falls through to the default
 *   route. That default is the attached guest: clockwork routes bytes, it
 *   does not interpret them. Adding an endpoint is one registerRoute(); not
 *   registering one is how a target that lacks a capability makes that traffic
 *   simply pass through.
 *
 *   src/clockwork_sys.h — the one address prefix clockwork keeps for itself. An
 *   address beginning "/clockwork/" (trailing slash included) is clockwork's
 *   and is answered here; anything else is forwarded untouched. clockwork_sys_claims()
 *   and handle_clockwork_sys_osc() are pure byte work — no globals, no device — so
 *   the whole rule is directly testable.
 *
 * The verbs under /clockwork/ are clockwork's own liveness surface: they name
 * nothing the guest owns, which is exactly why a test can use them to prove the
 * pipe carries bytes without borrowing anyone else's vocabulary. Addresses that
 * are not under that prefix appear below purely as filler — they exist only to
 * be routed, and mean nothing.
 */
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "OscTestUtils.h"
#include "src/OscIngress.h"
#include "src/clockwork_sys.h"

namespace {

// ── Router fixtures ──────────────────────────────────────────────────────────

// A sink records how many packets it received and the last payload length.
struct Recorder {
    int    calls   = 0;
    size_t lastLen = 0;
};

bool sink(void* ctx, const void* /*callCtx*/, const uint8_t* data, std::size_t len) {
    (void)data;
    auto* r = static_cast<Recorder*>(ctx);
    r->calls++;
    r->lastLen = len;
    return true;
}

// Ingest a NUL-terminated OSC address (len includes the terminator). The router
// classifies on the address alone, so this is a complete input for it.
void ingestAddr(const OscIngress& ix, const char* addr) {
    ix.ingest(reinterpret_cast<const uint8_t*>(addr),
              std::strlen(addr) + 1, nullptr);
}

// ── /clockwork/ fixtures ───────────────────────────────────────────────────────

// One captured reply: its parsed address plus the raw bytes, so blob and type
// tag can be read back without inventing an accessor the codec does not have.
struct Reply {
    std::string          address;
    std::vector<uint8_t> raw;
};

// Feed one packet to clockwork handler; collect whatever it emits.
struct Answer {
    bool               claimed = false;
    std::vector<Reply> replies;
};

Answer answer(const uint8_t* data, uint32_t len) {
    Answer a;
    a.claimed = handle_clockwork_sys_osc(data, len, [&a](const uint8_t* b, uint32_t n) {
        a.replies.push_back(Reply{ osc_test::parseReply(b, n).address,
                                   std::vector<uint8_t>(b, b + n) });
    });
    return a;
}

Answer answer(const osc_test::Packet& pkt) { return answer(pkt.ptr(), pkt.size()); }

// The OSC type-tag string of a message, without its leading ','. Read straight
// off the wire so the assertions below do not depend on the codec's own view.
std::string typeTagOf(const std::vector<uint8_t>& m) {
    const uint32_t n = static_cast<uint32_t>(m.size());
    uint32_t i = 0;
    while (i < n && m[i] != '\0') ++i;      // address
    i = (i + 4u) & ~3u;                     // padded past its NUL
    if (i >= n || m[i] != ',') return {};
    uint32_t e = i + 1;
    while (e < n && m[e] != '\0') ++e;
    return std::string(reinterpret_cast<const char*>(m.data()) + i + 1, e - (i + 1));
}

// The first blob argument's bytes, for a message whose only argument is a blob.
std::vector<uint8_t> blobOf(const std::vector<uint8_t>& m) {
    const uint32_t n = static_cast<uint32_t>(m.size());
    uint32_t i = 0;
    while (i < n && m[i] != '\0') ++i;
    i = (i + 4u) & ~3u;                     // start of the type tag
    uint32_t e = i;
    while (e < n && m[e] != '\0') ++e;
    uint32_t args = i + (((e - i) + 4u) & ~3u);
    if (args + 4u > n) return {};
    const uint32_t sz = (uint32_t(m[args]) << 24) | (uint32_t(m[args + 1]) << 16) |
                        (uint32_t(m[args + 2]) << 8) | uint32_t(m[args + 3]);
    if (args + 4u + sz > n) return {};
    return std::vector<uint8_t>(m.begin() + args + 4, m.begin() + args + 4 + sz);
}

// A route that hands claimed traffic to clockwork handler — the composition
// every ingress root performs.
struct SysRoute { std::vector<std::string> replies; };

bool sysRoute(void* ctx, const void* /*callCtx*/, const uint8_t* data, std::size_t len) {
    auto* r = static_cast<SysRoute*>(ctx);
    return handle_clockwork_sys_osc(data, static_cast<uint32_t>(len),
                              [r](const uint8_t* b, uint32_t n) {
        r->replies.push_back(osc_test::parseReply(b, n).address);
    });
}

} // namespace

// ════ OscIngress: the routing primitive ═════════════════════════════════════

TEST_CASE("Router: unclaimed messages and bundles go to the default sink",
          "[ingress][router]") {
    OscIngress ix;
    Recorder def, link;
    ix.setDefault(&sink, &def);
    ix.registerRoute(CLOCKWORK_SYS("clock/"), &sink, &link);

    // An arbitrary address claiming no registered prefix. It is filler: it
    // exists only to prove the mechanism carries bytes to the default route.
    ingestAddr(ix, "/test/filler");
    CHECK(def.calls == 1);
    CHECK(link.calls == 0);

    // A bundle is a schedule, and the schedule is the guest's — never a control
    // route, whatever prefixes are registered.
    const uint8_t bundle[16] = { '#','b','u','n','d','l','e','\0', 0,0,0,0,0,0,0,1 };
    ix.ingest(bundle, sizeof(bundle), nullptr);
    CHECK(def.calls == 2);
    CHECK(link.calls == 0);
}

TEST_CASE("Router: a registered control prefix peels off to its handler",
          "[ingress][router]") {
    OscIngress ix;
    Recorder def, link;
    ix.setDefault(&sink, &def);
    ix.registerRoute(CLOCKWORK_SYS("clock/"), &sink, &link);

    ingestAddr(ix, CLOCKWORK_SYS("clock/tempo/get"));
    CHECK(link.calls == 1);
    CHECK(def.calls == 0);

    // A sibling prefix that ISN'T registered falls through to the default —
    // this is capability-gating: a target without that subsystem simply never
    // registers it, and the traffic is forwarded like anything else.
    ingestAddr(ix, CLOCKWORK_SYS("devices/list"));
    CHECK(def.calls == 1);
    CHECK(link.calls == 1);
}

TEST_CASE("Router: the longest registered prefix wins", "[ingress][router]") {
    OscIngress ix;
    Recorder def, a, ab;
    ix.setDefault(&sink, &def);
    ix.registerRoute("/a/",   &sink, &a);
    ix.registerRoute("/a/b/", &sink, &ab);

    ingestAddr(ix, "/a/x");      // only /a/ matches
    CHECK(a.calls == 1);
    CHECK(ab.calls == 0);

    ingestAddr(ix, "/a/b/x");    // /a/b/ is longer → wins over /a/
    CHECK(ab.calls == 1);
    CHECK(a.calls == 1);
}

TEST_CASE("Router: a prefix without a trailing '/' claims whole segments only",
          "[ingress][router]") {
    OscIngress ix;
    Recorder def, ns, verb;
    ix.setDefault(&sink, &def);
    ix.registerRoute("/t/",      &sink, &ns);
    ix.registerRoute("/t/param", &sink, &verb);

    ingestAddr(ix, "/t/param");      // exactly the verb
    CHECK(verb.calls == 1);
    ingestAddr(ix, "/t/param/edit"); // the namespace under it
    CHECK(verb.calls == 2);
    // A longer sibling is a different verb: it stays with the namespace, so a
    // "params" listing never lands on the handler for "param" (which was how
    // the plugin-parameter listing got dropped on the audio thread).
    ingestAddr(ix, "/t/params");
    CHECK(verb.calls == 2);
    CHECK(ns.calls == 1);
    CHECK(def.calls == 0);
}

TEST_CASE("Router is modular: a prefix is reachable only once its route is registered",
          "[ingress][router][capability]") {
    OscIngress ix;
    Recorder def, link;
    ix.setDefault(&sink, &def);

    // Capability absent (route not registered): /clockwork/clock/ traffic just forwards.
    REQUIRE(ix.routeCount() == 0);
    ingestAddr(ix, CLOCKWORK_SYS("clock/tempo/get"));
    CHECK(def.calls == 1);
    CHECK(link.calls == 0);

    // Register the endpoint (one call) — the capability is now present.
    REQUIRE(ix.registerRoute(CLOCKWORK_SYS("clock/"), &sink, &link));
    REQUIRE(ix.routeCount() == 1);

    // Same traffic now reaches the handler instead of the default route.
    ingestAddr(ix, CLOCKWORK_SYS("clock/tempo/get"));
    CHECK(link.calls == 1);
    CHECK(def.calls == 1);
}

TEST_CASE("Router drops non-OSC packets without dispatching", "[ingress][router]") {
    OscIngress ix;
    Recorder def;
    ix.setDefault(&sink, &def);

    ingestAddr(ix, "not-an-address");     // no leading '/'
    const uint8_t tiny[2] = { '/', 0 };   // too short to be an address
    ix.ingest(tiny, sizeof(tiny), nullptr);

    CHECK(def.calls == 0);
}

// ════ clockwork_sys: the one prefix clockwork keeps ═════════════════════════════

TEST_CASE("ingress/clockwork-sys: the claim is exactly the \"/clockwork/\" prefix",
          "[ingress][clockwork_sys]") {
    auto claims = [](const char* addr) {
        return clockwork_sys_claims(reinterpret_cast<const uint8_t*>(addr),
                              static_cast<uint32_t>(std::strlen(addr) + 1));
    };

    CHECK(claims("/clockwork/ping"));
    CHECK(claims("/clockwork/echo"));
    CHECK(claims("/clockwork/anything/at/all"));
    CHECK(claims("/clockwork/"));           // the bare prefix is still clockwork's

    // Not claimed: "/clockwork" without the trailing slash is a different
    // address component, and so is a longer name that merely starts with the
    // same letters.
    CHECK_FALSE(claims("/clockwork"));
    CHECK_FALSE(claims("/clockworks/foo"));
    CHECK_FALSE(claims("/clockwork-sys/foo"));
    CHECK_FALSE(claims("/test/filler"));

    // Degenerate inputs are refused rather than read.
    CHECK_FALSE(clockwork_sys_claims(nullptr, 64));
    CHECK_FALSE(claims(""));
    const uint8_t shorter[4] = { '/', 'c', 'l', 'o' };
    CHECK_FALSE(clockwork_sys_claims(shorter, sizeof shorter));
}

TEST_CASE("ingress/clockwork-sys: an unclaimed address is left alone entirely",
          "[ingress][clockwork_sys]") {
    // The address is arbitrary filler; the point is that the handler neither
    // answers it nor emits anything, so it is still the guest's to receive.
    auto a = answer(osc_test::message("/test/filler", (int32_t)1));
    CHECK_FALSE(a.claimed);
    CHECK(a.replies.empty());

    auto b = answer(osc_test::message("/clockwork-system-status"));
    CHECK_FALSE(b.claimed);
    CHECK(b.replies.empty());
}

TEST_CASE("ingress/clockwork-sys: /clockwork/ping answers /clockwork/pong", "[ingress][clockwork_sys]") {
    SECTION("with no argument") {
        auto a = answer(osc_test::message("/clockwork/ping"));
        CHECK(a.claimed);
        REQUIRE(a.replies.size() == 1);
        CHECK(a.replies[0].address == "/clockwork/pong");
        CHECK(typeTagOf(a.replies[0].raw).empty());
    }

    SECTION("echoing the caller's int32 id") {
        auto a = answer(osc_test::message("/clockwork/ping", (int32_t)4242));
        CHECK(a.claimed);
        REQUIRE(a.replies.size() == 1);
        auto parsed = osc_test::parseReply(a.replies[0].raw.data(),
                                           static_cast<uint32_t>(a.replies[0].raw.size()));
        CHECK(parsed.address == "/clockwork/pong");
        REQUIRE(parsed.argCount() == 1);
        CHECK(parsed.argInt(0) == 4242);
    }

    SECTION("a non-int argument is dropped, not echoed") {
        auto a = answer(osc_test::message("/clockwork/ping", "not-an-id"));
        CHECK(a.claimed);
        REQUIRE(a.replies.size() == 1);
        CHECK(a.replies[0].address == "/clockwork/pong");
        CHECK(typeTagOf(a.replies[0].raw).empty());
    }
}

TEST_CASE("ingress/clockwork-sys: /clockwork/echo returns its string argument",
          "[ingress][clockwork_sys]") {
    auto a = answer(osc_test::message("/clockwork/echo", "the pipe carries bytes"));
    CHECK(a.claimed);
    REQUIRE(a.replies.size() == 1);

    auto parsed = osc_test::parseReply(a.replies[0].raw.data(),
                                       static_cast<uint32_t>(a.replies[0].raw.size()));
    CHECK(parsed.address == "/clockwork/echo.reply");
    CHECK(typeTagOf(a.replies[0].raw) == "s");
    REQUIRE(parsed.argCount() == 1);
    CHECK(parsed.argString(0) == "the pipe carries bytes");
}

TEST_CASE("ingress/clockwork-sys: /clockwork/echo returns its blob argument byte for byte",
          "[ingress][clockwork_sys]") {
    // Arbitrary bytes, including a NUL and a high bit, so a string-shaped
    // shortcut in the reply path would be caught.
    const uint8_t payload[] = {0x00, 0x01, 0xFF, 0x7F, 0x80, 'x', 0x00, 0x2A, 0x2A};
    auto a = answer(osc_test::messageWithBlob("/clockwork/echo", payload, sizeof payload));
    CHECK(a.claimed);
    REQUIRE(a.replies.size() == 1);
    CHECK(a.replies[0].address == "/clockwork/echo.reply");
    CHECK(typeTagOf(a.replies[0].raw) == "b");

    const auto got = blobOf(a.replies[0].raw);
    REQUIRE(got.size() == sizeof payload);
    CHECK(std::memcmp(got.data(), payload, sizeof payload) == 0);
}

TEST_CASE("ingress/clockwork-sys: an unknown verb is refused, never forwarded",
          "[ingress][clockwork_sys]") {
    // The prefix is claimed as a whole. Falling through to the guest would make
    // the rule depend on which verbs clockwork happens to implement today.
    auto a = answer(osc_test::message("/clockwork/no-such-verb"));
    CHECK(a.claimed);                      // claimed, and answered here
    REQUIRE(a.replies.size() == 1);

    auto parsed = osc_test::parseReply(a.replies[0].raw.data(),
                                       static_cast<uint32_t>(a.replies[0].raw.size()));
    CHECK(parsed.address == "/clockwork/error");
    CHECK(typeTagOf(a.replies[0].raw) == "ss");
    REQUIRE(parsed.argCount() == 2);
    CHECK(parsed.argString(0) == "/clockwork/no-such-verb");   // the address at fault
    CHECK_FALSE(parsed.argString(1).empty());                // and a reason
}

TEST_CASE("ingress/clockwork-sys: /clockwork/echo without a string or blob is refused",
          "[ingress][clockwork_sys]") {
    SECTION("no argument at all") {
        auto a = answer(osc_test::message("/clockwork/echo"));
        CHECK(a.claimed);
        REQUIRE(a.replies.size() == 1);
        CHECK(a.replies[0].address == "/clockwork/error");
    }

    SECTION("an argument of the wrong type") {
        auto a = answer(osc_test::message("/clockwork/echo", (int32_t)1));
        CHECK(a.claimed);
        REQUIRE(a.replies.size() == 1);
        auto parsed = osc_test::parseReply(a.replies[0].raw.data(),
                                           static_cast<uint32_t>(a.replies[0].raw.size()));
        CHECK(parsed.address == "/clockwork/error");
        REQUIRE(parsed.argCount() == 2);
        CHECK(parsed.argString(0) == "/clockwork/echo");
    }
}

TEST_CASE("ingress/clockwork-sys: a claimed but malformed packet answers an error, not a crash",
          "[ingress][clockwork_sys]") {
    // "/clockwork/zzz" padded, then a type tag declaring an int32 that isn't
    // there. Whether the decode throws or the verb is simply unknown, the
    // answer is the same shape: an error under clockwork's own prefix.
    std::vector<uint8_t> bad;
    const char* addr = "/clockwork/zzz";
    bad.insert(bad.end(), addr, addr + std::strlen(addr));
    bad.push_back(0);
    while (bad.size() % 4u) bad.push_back(0);
    const char* tag = ",i";
    bad.insert(bad.end(), tag, tag + std::strlen(tag));
    bad.push_back(0);
    while (bad.size() % 4u) bad.push_back(0);
    // …and no int32 follows.

    auto a = answer(bad.data(), static_cast<uint32_t>(bad.size()));
    CHECK(a.claimed);
    REQUIRE(a.replies.size() == 1);
    CHECK(a.replies[0].address == "/clockwork/error");
}

// ════ OscSplit: the one predicate ════════════════════════════════════════════
//
// This is the rule the whole repository is organised around, so it is pinned
// from the outside: bytes in, and nothing but the address deciding which side
// they land on.

TEST_CASE("boundary: the prefix is the only thing that decides clockwork vs DSP",
          "[ingress][boundary][clockwork_sys]") {
    OscSplit      boundary;
    ClockworkSysRoutes sysRoutes;
    Recorder     dsp;             // stands in for the guest: everything else
    SysRoute     answered;

    sysRoutes.setFallback(&sysRoute, &answered);
    boundary.setSys(&ClockworkSysRoutes::route, &sysRoutes);
    boundary.setDsp(&sink, &dsp);

    auto send = [&](const char* addr) {
        const auto m = osc_test::message(addr, (int32_t)1);
        return boundary.ingest(m.ptr(), m.size(), nullptr);
    };

    // -- Under the prefix: clockwork's, answered here ----------------------
    CHECK(send(CLOCKWORK_SYS("ping")));
    REQUIRE(answered.replies.size() == 1);
    CHECK(answered.replies[0] == CLOCKWORK_SYS("pong"));
    CHECK(dsp.calls == 0);

    // A verb clockwork does not implement is still clockwork's. It is
    // REFUSED, not forwarded — otherwise the rule would depend on which verbs
    // happen to exist today, and adding one would silently take an address away
    // from the DSP.
    CHECK(send(CLOCKWORK_SYS("no-such-verb")));
    REQUIRE(answered.replies.size() == 2);
    CHECK(answered.replies[1] == CLOCKWORK_SYS("error"));
    CHECK(dsp.calls == 0);

    // -- Outside the prefix: the DSP's, untouched ----------------------------
    // Near misses first. Each of these is one character away from being
    // clockwork's and none of them is.
    const char* forwarded[] = {
        "/clockwork-system/x",        // longer component, not the prefix
        "/clockwork-system-status",
        "/clockwork-sys",             // no trailing slash: a different component
        "/clockwork-sysx/ping",
        "/clockworks/ping",           // near-miss: the prefix needs its slash
        // THE REGRESSION GUARD. Every one of these was a clockwork verb before
        // the namespaces moved under the prefix. They must now reach the DSP
        // like any other address: if one of these is still answered here, the
        // migration left a second predicate behind.
        "/clock/tempo/get",
        "/engine/record/start",
        "/midi/out/note_on",
        "/gamepad/in/button",
        "/osc/send",
        "/schedule",
        "/sched/flush",
        "/linkage",
    };
    int expected = 0;
    for (const char* addr : forwarded) {
        CHECK(send(addr));
        CHECK(dsp.calls == ++expected);
        // …and clockwork said nothing at all about any of them.
        CHECK(answered.replies.size() == 2);
    }

    // A bundle is a schedule, and the schedule is the DSP's, whatever it holds.
    const uint8_t bundle[16] = { '#','b','u','n','d','l','e','\0', 0,0,0,0,0,0,0,1 };
    CHECK(boundary.ingest(bundle, sizeof(bundle), nullptr));
    CHECK(dsp.calls == expected + 1);
    CHECK(answered.replies.size() == 2);
}

TEST_CASE("boundary: a clockwork route cannot be spelled outside the prefix",
          "[ingress][boundary][clockwork_sys]") {
    // The structural half of the rule. ClockworkSysRoutes::add takes the part of the
    // address that FOLLOWS the reserved prefix and prepends it here, so there
    // is no parameter in which a second namespace could be introduced — an
    // absolute address handed to it becomes a path UNDER the prefix, which is
    // harmless, rather than a rival route beside it.
    OscSplit      boundary;
    ClockworkSysRoutes sysRoutes;
    Recorder     dsp, mistake;
    SysRoute     answered;

    // Someone tries to register "/clock/" the old way.
    REQUIRE(sysRoutes.add("/clock/", &sink, &mistake));
    sysRoutes.setFallback(&sysRoute, &answered);
    boundary.setSys(&ClockworkSysRoutes::route, &sysRoutes);
    boundary.setDsp(&sink, &dsp);

    // What they actually registered is "/clockwork//clock/" — inside the prefix.
    // The old address is untouched and still the DSP's.
    const auto old = osc_test::message("/clock/tempo/get", (int32_t)1);
    CHECK(boundary.ingest(old.ptr(), old.size(), nullptr));
    CHECK(dsp.calls == 1);
    CHECK(mistake.calls == 0);

    // The bare prefix is refused outright: an empty suffix would match every
    // verb and turn clockwork's internal table back into a second predicate.
    CHECK_FALSE(sysRoutes.add("", &sink, &mistake));
}

TEST_CASE("ingress/clockwork-sys: the prefix constant matches its documented shape",
          "[ingress][clockwork_sys]") {
    // Derived from the ONE definition, so changing the prefix is one line in
    // clockwork_prefix.h and not a sweep. What is pinned here is the SHAPE the rule
    // depends on, not the spelling: a leading slash so it is an address, and a
    // trailing slash so "/clockwork-sys" and "/clockwork-system-status" are not claimed.
    const std::string prefix = CLOCKWORK_SYS_PREFIX;
    CHECK(prefix == CLOCKWORK_SYS_PREFIX_LIT);
    CHECK(CLOCKWORK_SYS_PREFIX_LEN == prefix.size());
    CHECK(CLOCKWORK_SYS_PREFIX_LEN >= 3u);
    CHECK(CLOCKWORK_SYS_PREFIX[0] == '/');
    CHECK(CLOCKWORK_SYS_PREFIX[CLOCKWORK_SYS_PREFIX_LEN - 1] == '/');
    CHECK(prefix.find('/', 1) == CLOCKWORK_SYS_PREFIX_LEN - 1);   // exactly one component

    // CLOCKWORK_SYS() and CLOCKWORK_SYS_LEN() agree with it.
    CHECK(std::string(CLOCKWORK_SYS("ping")) == prefix + "ping");
    CHECK(CLOCKWORK_SYS_LEN("ping") == prefix.size() + 4u);
    CHECK(CLOCKWORK_SYS_PADDED("ping") == ((CLOCKWORK_SYS_LEN("ping") + 4u) & ~3u));
}
