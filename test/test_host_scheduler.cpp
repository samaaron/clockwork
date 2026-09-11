// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_host_scheduler.cpp — the standalone scheduler host's logic:
 * src/host/host_scheduler.h, src/host/host_outbound.h and src/host/osc_reader.h.
 *
 * Brought across from supersonic's test/host/host_tests.cpp, where it ran as
 * its own main() against the same three headers. Here it is Catch2, the
 * messages come from oscpack rather than a hand encoder, and the addresses
 * are the ones the host actually answers to now — /clockwork/schedule,
 * /clockwork/sched/flush, /clockwork/osc/send, /clockwork/midi/....
 *
 * The host is HostScheduler::ingest() from a recv thread and tick() from the
 * one thread that owns the store; between them the generic Scheduler, and on
 * fire the host's own route table: osc/send to the OSC leaf, midi/ to the
 * MIDI leaf, anything else reported and dropped. No socket and no wall clock:
 * the timetags are plain int64 and tick() is handed the "now" to compare
 * them with, so "due" is exact.
 */
#include "clockwork_prefix.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "OscTestUtils.h"
#include "clockwork_sys.h"
#include "host/host_scheduler.h"
#include "host/host_outbound.h"
#include "host/osc_reader.h"

namespace {

struct OscSend {
    std::string          host;
    int                  port;
    std::vector<uint8_t> inner;
};

// The host stood up with recording leaves in place of the OSC and MIDI
// subsystems: every fired event lands in one of the two vectors, or nowhere.
struct Rig {
    std::vector<OscSend>              oscSends;
    std::vector<std::vector<uint8_t>> midiSends;
    clockwork_host::HostSenders       senders;
    ClockworkSysRoutes                routes;
    clockwork_host::HostScheduler     sched;

    Rig()
        : senders{
              [this](const char* host, int port, const uint8_t* inner, uint32_t len) {
                  oscSends.push_back({host, port, std::vector<uint8_t>(inner, inner + len)});
              },
              [this](const uint8_t* inner, uint32_t len) {
                  midiSends.push_back(std::vector<uint8_t>(inner, inner + len));
              }},
          sched(routes)
    {
        // Exactly the wiring in src/host/main.cpp.
        routes.add("osc/send", &clockwork_host::hostOscSendRoute, &senders);
        routes.add("midi/",    &clockwork_host::hostMidiRoute,    &senders);
        routes.setFallback(&clockwork_host::hostUnroutedRoute, nullptr);
    }

    void ingest(const osc_test::Packet& p) { sched.ingest(p.ptr(), p.size()); }
};

// "/clockwork/osc/send <host:s> <port:i> <inner:b>" — what a fired event
// carries to the OSC leaf.
osc_test::Packet oscSend(const char* host, int port, const std::vector<uint8_t>& inner) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("osc/send"))
        << host << port
        << osc::Blob(inner.data(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    return b.end();
}

// "/clockwork/schedule <when:h> <inner:b>" — the control verb the host queues.
osc_test::Packet schedule(int64_t when, const osc_test::Packet& inner) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("schedule"))
        << static_cast<osc::int64>(when)
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    return b.end();
}

osc_test::Packet oscAt(int64_t when, const char* host, int port, const std::vector<uint8_t>& inner) {
    return schedule(when, oscSend(host, port, inner));
}

// A "/clockwork/midi/..." message, as a client schedules one: the host does
// not parse it, it hands the whole message to the MIDI leaf.
osc_test::Packet midiMsg(const std::vector<uint8_t>& bytes) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/out/raw"))
        << osc::Blob(bytes.data(), static_cast<osc::osc_bundle_element_size_t>(bytes.size()));
    return b.end();
}

osc_test::Packet schedFlush(const char* tag) {
    return osc_test::message(CLOCKWORK_SYS("sched/flush"), tag);
}

bool same(const std::vector<uint8_t>& a, const osc_test::Packet& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.ptr(), a.size()) == 0;
}

}  // namespace

// ── OscReader ────────────────────────────────────────────────────────────────

TEST_CASE("host scheduler: OscReader reads the osc/send fields in order",
          "[host_scheduler][osc_reader]") {
    const std::vector<uint8_t> inner = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
    const auto msg = oscSend("127.0.0.1", 4560, inner);

    clockwork_host::OscReader r(msg.ptr(), msg.size());
    REQUIRE(r.ok());
    CHECK(std::strcmp(r.address(), CLOCKWORK_SYS("osc/send")) == 0);

    const char* host = nullptr;
    REQUIRE(r.readString(host));
    CHECK(std::strcmp(host, "127.0.0.1") == 0);

    int32_t port = 0;
    REQUIRE(r.readInt32(port));
    CHECK(port == 4560);

    const uint8_t* blob = nullptr;
    uint32_t       blobLen = 0;
    REQUIRE(r.readBlob(blob, blobLen));
    REQUIRE(blobLen == inner.size());
    CHECK(std::memcmp(blob, inner.data(), blobLen) == 0);
}

TEST_CASE("host scheduler: a malformed packet reads as not ok, and no read succeeds",
          "[host_scheduler][osc_reader]") {
    // "/x" and then garbage: no NUL-terminated type tag anywhere.
    const uint8_t junk[6] = {0x2f, 0x78, 0x01, 0x02, 0x03, 0x04};
    clockwork_host::OscReader r(junk, sizeof junk);
    CHECK_FALSE(r.ok());
}

// ── Scheduling ───────────────────────────────────────────────────────────────

TEST_CASE("host scheduler: a scheduled osc/send fires only when due, whole",
          "[host_scheduler]") {
    Rig rig;
    const std::vector<uint8_t> inner = {1, 2, 3, 4};
    rig.ingest(oscAt(1000, "10.0.0.5", 9000, inner));

    rig.sched.tick(999);
    CHECK(rig.oscSends.empty());           // not yet due
    CHECK(rig.sched.pending() == 1);

    rig.sched.tick(1000);                  // due now
    REQUIRE(rig.oscSends.size() == 1);
    CHECK(rig.oscSends[0].host == "10.0.0.5");
    CHECK(rig.oscSends[0].port == 9000);
    CHECK(rig.oscSends[0].inner == inner);
    CHECK(rig.sched.pending() == 0);
}

TEST_CASE("host scheduler: time order wins over arrival order", "[host_scheduler]") {
    Rig rig;
    rig.ingest(oscAt(3000, "h", 1, {0xAA}));   // later, queued first
    rig.ingest(oscAt(2000, "h", 2, {0xBB}));   // earlier, queued second

    rig.sched.tick(5000);
    REQUIRE(rig.oscSends.size() == 2);
    CHECK(rig.oscSends[0].port == 2);          // t=2000 first
    CHECK(rig.oscSends[1].port == 1);          // t=3000 second
}

TEST_CASE("host scheduler: sched/flush \"default\" cancels what has not fired",
          "[host_scheduler]") {
    Rig rig;
    rig.ingest(oscAt(4000, "h", 7, {0x09}));
    rig.ingest(schedFlush("default"));

    rig.sched.tick(9000);
    CHECK(rig.oscSends.empty());
    CHECK(rig.sched.pending() == 0);
}

TEST_CASE("host scheduler: a scheduled midi message reaches the MIDI leaf when due",
          "[host_scheduler]") {
    Rig rig;
    const auto note = midiMsg({0x90, 0x40, 0x7f});
    rig.ingest(schedule(6000, note));

    rig.sched.tick(7000);
    REQUIRE(rig.midiSends.size() == 1);
    CHECK(same(rig.midiSends[0], note));
}

TEST_CASE("host scheduler: an out-of-range port is refused, not wrapped",
          "[host_scheduler]") {
    Rig rig;
    rig.ingest(oscAt(1000, "h", 70000, {0x01}));   // > 65535
    rig.ingest(oscAt(1000, "h", 4560,  {0x02}));

    rig.sched.tick(2000);
    REQUIRE(rig.oscSends.size() == 1);             // only the valid port went out
    CHECK(rig.oscSends[0].port == 4560);
}

TEST_CASE("host scheduler: midi/ routes by prefix, never to the OSC leaf",
          "[host_scheduler]") {
    Rig rig;
    const auto midi = midiMsg({0x40, 0x7f, 0x10});
    rig.ingest(schedule(8000, midi));

    rig.sched.tick(9000);
    CHECK(rig.oscSends.empty());
    REQUIRE(rig.midiSends.size() == 1);
    CHECK(same(rig.midiSends[0], midi));
}

TEST_CASE("host scheduler: a verb with no leaf here is dropped, not misrouted",
          "[host_scheduler]") {
    // A DSP verb scheduled on a host with no DSP: the fallback reports and
    // drops it, and neither leaf sees anything.
    Rig rig;
    osc_test::Builder b;
    b.begin("/s_new") << "beep" << 1;
    rig.ingest(schedule(8000, b.end()));

    rig.sched.tick(9000);
    CHECK(rig.oscSends.empty());
    CHECK(rig.midiSends.empty());
    CHECK(rig.sched.pending() == 0);
}
