// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_host_scheduler.cpp — the standalone scheduler host's logic:
 * src/host/host_scheduler.h, src/host/host_outbound.h and src/host/osc_reader.h.
 *
 * The messages come from oscpack, and the addresses are the ones the host
 * answers to — /clockwork/schedule, /clockwork/sched/flush,
 * /clockwork/osc/send, /clockwork/midi/....
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
#include "host/clock.h"
#include "host/host_scheduler.h"
#include "host/host_outbound.h"
#include "host/osc_reader.h"
#include "workers/thread_name.h"
#include "ThreadWakes.h"

#include <atomic>
#include <mutex>
#include <stop_token>
#include <thread>

namespace {

struct OscSend {
    std::string          host;
    int                  port;
    std::vector<uint8_t> inner;
};

// The host stood up with recording leaves in place of the OSC and MIDI
// subsystems: every fired event lands in one of the two vectors, or nowhere.
// An OSC send also notes when it fired, under a lock, for the cases where the
// store runs on a thread of its own.
struct Rig {
    std::mutex                        mx;
    std::vector<int64_t>              firedAt;
    std::vector<OscSend>              oscSends;
    std::vector<std::vector<uint8_t>> midiSends;
    clockwork_host::HostSenders       senders;
    ClockworkSysRoutes                routes;
    clockwork_host::HostScheduler     sched;

    Rig()
        : senders{
              [this](const char* host, int port, const uint8_t* inner, uint32_t len) {
                  std::lock_guard<std::mutex> lk(mx);
                  oscSends.push_back({host, port, std::vector<uint8_t>(inner, inner + len)});
                  firedAt.push_back(clockwork_host::osc_now());
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

// ── What the scheduler thread waits for ──────────────────────────────────────
//
// The thread that runs the store sleeps until the next event is due or a
// message arrives, whichever is first, and never looks in between.

TEST_CASE("host scheduler: every message ingested wakes the thread that runs the store",
          "[host_scheduler]") {
    Rig rig;
    const uint32_t before = rig.sched.arrivals().value.load();
    rig.ingest(oscAt(1000, "h", 1, {0x01}));
    CHECK(rig.sched.arrivals().value.load() == before + 1);
    rig.ingest(schedFlush("default"));
    CHECK(rig.sched.arrivals().value.load() == before + 2);

    // A message that is not a verb here is not work, and wakes nothing.
    const uint8_t junk[6] = {0x2f, 0x78, 0x01, 0x02, 0x03, 0x04};
    rig.sched.ingest(junk, sizeof junk);
    CHECK(rig.sched.arrivals().value.load() == before + 2);
}

TEST_CASE("host scheduler: the next wake is when the earliest held event is due, and none with nothing held",
          "[host_scheduler]") {
    Rig rig;
    rig.sched.tick(0);
    CHECK(rig.sched.nextDue() == INT64_MAX);

    rig.ingest(oscAt(3000, "h", 1, {0xAA}));
    rig.ingest(oscAt(2000, "h", 2, {0xBB}));
    rig.sched.tick(1000);                  // takes both in, fires neither
    CHECK(rig.sched.nextDue() == 2000);

    rig.sched.tick(2000);
    CHECK(rig.sched.nextDue() == 3000);
    rig.sched.tick(3000);
    CHECK(rig.sched.nextDue() == INT64_MAX);
}

TEST_CASE("host scheduler: the wait to a timetag is rounded up to a whole microsecond, and none for one already past",
          "[host_scheduler]") {
    using std::chrono::microseconds;
    constexpr int64_t kSecond = int64_t(1) << 32;
    CHECK(clockwork_host::osc_interval(kSecond) == microseconds(1'000'000));
    CHECK(clockwork_host::osc_interval(3 * kSecond + kSecond / 2) == microseconds(3'500'000));
    CHECK(clockwork_host::osc_interval(1) == microseconds(1));
    // An hour on, where ticks times a million would not fit in 64 bits.
    CHECK(clockwork_host::osc_interval(3600 * kSecond) == microseconds(3'600'000'000));
    CHECK(clockwork_host::osc_interval(0) == microseconds(0));
    CHECK(clockwork_host::osc_interval(-kSecond) == microseconds(0));
}

// ── The store's thread ───────────────────────────────────────────────────────

namespace {

constexpr int64_t kTimetagSecond = int64_t(1) << 32;

// Waits until the rig has fired `n` OSC sends, for at most ten seconds.
bool firesReach(Rig& rig, size_t n) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard<std::mutex> lk(rig.mx);
            if (rig.oscSends.size() >= n) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

TEST_CASE("host scheduler: the store's thread fires each event at its time, takes each message as it arrives, "
          "and stops when asked", "[host_scheduler][thread]") {
    Rig rig;
    std::jthread thread([&rig](const std::stop_token& stop) { rig.sched.run(stop); });

    // An event a tenth of a second ahead fires then: not before, and not a
    // second late.
    const int64_t due = clockwork_host::osc_now() + kTimetagSecond / 10;
    rig.ingest(oscAt(due, "h", 1, {0x01}));
    REQUIRE(firesReach(rig, 1));
    {
        std::lock_guard<std::mutex> lk(rig.mx);
        CHECK(rig.firedAt[0] >= due);
        CHECK(rig.firedAt[0] - due < kTimetagSecond);
    }

    // An event already due, with nothing held: only its arrival can wake the
    // thread.
    rig.ingest(oscAt(clockwork_host::osc_now(), "h", 2, {0x02}));
    REQUIRE(firesReach(rig, 2));

    // With an event a minute ahead, a stop ends the thread at once.
    rig.ingest(oscAt(clockwork_host::osc_now() + 60 * kTimetagSecond, "h", 3, {0x03}));
    const auto t0 = std::chrono::steady_clock::now();
    thread.request_stop();
    thread.join();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
    std::lock_guard<std::mutex> lk(rig.mx);
    CHECK(rig.oscSends.size() == 2);
}

TEST_CASE("host scheduler: the store's thread does not wake while it waits", "[host_scheduler][thread][wakes]") {
    if (!thread_wakes::countable()) SKIP("per-thread wake counts are read from Linux's /proc");
    Rig rig;
    std::jthread thread([&rig](const std::stop_token& stop) {
        clockwork::name_this_thread("cw-scheduler");
        rig.sched.run(stop);
    });
    // First nothing held, then an event a minute ahead. A thread that ticked
    // every millisecond would wake a thousand times a second either way.
    auto woke = thread_wakes::over("cw-scheduler", std::chrono::milliseconds(500));
    REQUIRE(woke);
    CHECK(*woke <= 1);
    rig.ingest(oscAt(clockwork_host::osc_now() + 60 * kTimetagSecond, "h", 1, {0x01}));
    woke = thread_wakes::over("cw-scheduler", std::chrono::milliseconds(500));
    REQUIRE(woke);
    CHECK(*woke <= 2);   // the arrival, and nothing for a second after
}

TEST_CASE("host scheduler: the store's thread follows a wall clock set forward while it sleeps",
          "[host_scheduler][thread]") {
    // Timetags use the wall clock and the sleep uses the steady clock. Setting
    // the wall clock forward an hour (a user, a first NTP sync) makes an event
    // an hour ahead due now; a thread asleep for the hour would miss it.
    Rig rig;
    std::atomic<int64_t> setForward{0};
    std::jthread thread([&rig, &setForward](const std::stop_token& stop) {
        rig.sched.run(stop, [&setForward] { return clockwork_host::osc_now() + setForward.load(); });
    });
    rig.ingest(oscAt(clockwork_host::osc_now() + 3600 * kTimetagSecond, "h", 1, {0x01}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // now asleep, the event held
    {
        std::lock_guard<std::mutex> lk(rig.mx);
        REQUIRE(rig.oscSends.empty());
    }
    setForward.store(3600 * kTimetagSecond);
    CHECK(firesReach(rig, 1));
}
