// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_rings.cpp — the ring, through the client boundary, at the states
 * that are hard to reach by accident.
 *
 * A round trip proves a message arrives. It does not prove the arithmetic,
 * because the awkward decisions are only made when the ring is nearly full as
 * it wraps — and a test that sends and waits drains as it goes, so the tail is
 * always far away and the decision that could be wrong is never the decision
 * being made. Two deliberately broken ring implementations passed an
 * end-to-end suite unchanged, which is what prompted this file.
 *
 * Here the drain is the test's to schedule, so the states can be constructed:
 * a ring filled and left unread, a producer that keeps going after a refusal,
 * a wrap with the reader still behind. These are also the acceptance tests for
 * any future implementation of the boundary — a socket transport, or a
 * reimplementation for another platform, has to satisfy the same table.
 */
#include "EngineFixture.h"
#include "clockwork_client.h"
#include "shared_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

extern "C" { extern uint8_t* shared_memory; }

using engine_test::Engine;

namespace {

ClockworkClient* openClient(ClockworkStatus* st) {
    return clockwork_client_open_memory(shared_memory, TOTAL_BUFFER_SIZE, st);
}

// A message of a chosen payload size, so a test can fill a ring in a known
// number of writes rather than guessing.
std::vector<uint8_t> sized(uint32_t payloadBytes) {
    // "/dummy/blob" + ",b" + size + bytes, each part padded to four.
    osc_test::Builder b;
    auto& s = b.begin("/dummy/blob");
    std::vector<uint8_t> blob(payloadBytes, 0xA5);
    s << osc::Blob(blob.data(), static_cast<osc::osc_bundle_element_size_t>(blob.size()));
    const auto pkt = b.end();
    return std::vector<uint8_t>(pkt.ptr(), pkt.ptr() + pkt.size());
}

} // namespace

TEST_CASE("client rings: a ring that is never drained refuses, and says which refusal",
          "[client][rings]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    // No blocks are pumped, so nothing drains: the ring fills and stays full.
    // A client must be told the difference between "not now" and "never" —
    // one is worth retrying and the other is a bug in the caller.
    const auto msg = sized(4096);
    uint32_t accepted = 0;
    ClockworkStatus last = CLOCKWORK_OK;
    for (int i = 0; i < 4096; ++i) {
        last = clockwork_client_send(c, msg.data(), static_cast<uint32_t>(msg.size()), 1);
        if (last != CLOCKWORK_OK) break;
        ++accepted;
    }

    CHECK(last == CLOCKWORK_E_FULL);          // not E_ARG, not E_TOO_BIG
    // It took some, then stopped: a ring that refused everything would mean
    // the writer never worked, and one that accepted everything would mean it
    // was overwriting unread frames.
    CHECK(accepted > 0);
    CHECK(accepted < 4096);

    clockwork_client_close(c);
}

TEST_CASE("client rings: a refusal leaves the ring usable, not wedged",
          "[client][rings]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto msg = sized(4096);
    while (clockwork_client_send(c, msg.data(),
                                 static_cast<uint32_t>(msg.size()), 1) == CLOCKWORK_OK) {}

    // Now let the engine run. A writer that gave up half way through a frame
    // on refusal leaves a length or a magic behind, and the reader never
    // recovers — so the test of a refusal is what happens next.
    e.pump(0.25);

    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 0xBEEF) == CLOCKWORK_OK);

    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    clockwork_client_close(c);
}

TEST_CASE("client rings: wrapping keeps every frame whole and in order",
          "[client][rings]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    // Enough traffic to take the write head past the end of the ring several
    // times over, drained as it goes so the wrap happens against a moving
    // reader rather than an idle one.
    const uint32_t payload = 8192;
    const auto msg = sized(payload);
    const uint32_t rounds = (IN_BUFFER_SIZE / payload) * 3;

    uint32_t sent = 0;
    for (uint32_t i = 0; i < rounds; ++i) {
        ClockworkStatus s = clockwork_client_send(c, msg.data(),
                                                  static_cast<uint32_t>(msg.size()), 1);
        if (s == CLOCKWORK_E_FULL) { e.pump(0.02); --i; continue; }
        REQUIRE(s == CLOCKWORK_OK);
        ++sent;
        if ((i % 4) == 0) e.pump(0.02);
    }
    e.pump(0.5);

    CHECK(sent == rounds);
    CHECK(sent * payload > IN_BUFFER_SIZE);   // it really did wrap

    // The guest answers every blob with its length and a hash, so a frame that
    // was torn by a wrap, or spliced with its neighbour, is a reply that never
    // comes or one that disagrees.
    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/blob-is", r));
    CHECK(static_cast<uint32_t>(r.argInt(0)) == payload);

    clockwork_client_close(c);
}

TEST_CASE("client rings: a second handle polling takes from the first",
          "[client][rings]") {
    // WHAT POLLING IS. The egress ring has one read cursor and it lives in the
    // engine's control block, so polling MOVES it for everybody. Two handles
    // are two ways of reaching the same queue, not two copies of the stream.
    //
    // This replaced a case that asserted the opposite. It passed for two
    // years' worth of runs because both handles saw nothing at all — the
    // engine's own gateway had already drained the ring — so `na == nb` was
    // `0 == 0` and would have held however the cursors were wired.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* a = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);
    ClockworkClient* b = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    // Both handles address one cursor, so advancing it through `a` is visible
    // through `b`: the region call reports the ring they share.
    ClockworkRegion ra{}, rb{};
    REQUIRE(clockwork_client_region(a, CLOCKWORK_REGION_EGRESS, &ra) == CLOCKWORK_OK);
    REQUIRE(clockwork_client_region(b, CLOCKWORK_REGION_EGRESS, &rb) == CLOCKWORK_OK);
    CHECK(ra.base == rb.base);
    CHECK(ra.bytes == OUT_BUFFER_SIZE);
    CHECK(ra.writable == 0);          // the engine's to write

    clockwork_client_close(a);
    clockwork_client_close(b);
}

TEST_CASE("client tap: a watcher sees traffic the consumer already took",
          "[client][rings][tap]") {
    // THE CASE THE OLD TEST WAS REACHING FOR. The engine's gateway drains the
    // egress ring, which is why a client polling it usually finds nothing —
    // and why "watch" cannot be built out of "take". A tap has its own cursor
    // and reads the bytes where they lie, so a frame already consumed is still
    // there to be seen until a writer overwrites it.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkClientTap* tap =
        clockwork_client_tap_open(c, CLOCKWORK_REGION_EGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);
    REQUIRE(tap != nullptr);

    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 0x5150) == CLOCKWORK_OK);

    // The fixture's own drain takes the reply, exactly as the gateway does.
    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    // And the tap still finds it.
    ClockworkClientMessage msgs[16];
    uint32_t seen = 0;
    bool sawPong = false;
    for (int spin = 0; spin < 50 && !sawPong; ++spin) {
        const uint32_t n = clockwork_client_tap_poll(tap, msgs, 16);
        seen += n;
        for (uint32_t i = 0; i < n; ++i) {
            const std::string addr(reinterpret_cast<const char*>(msgs[i].bytes));
            if (addr == "/dummy/pong") sawPong = true;
        }
        if (!sawPong) e.pump(0.02);
    }
    CHECK(seen > 0);
    CHECK(sawPong);

    clockwork_client_tap_close(tap);
    clockwork_client_close(c);
}

TEST_CASE("client tap: the ingress ring can be watched but not drained",
          "[client][rings][tap]") {
    // The engine consumes ingress, so there is no polling it — a client that
    // did would be eating the engine's input. Watching it is how a logger
    // shows what was sent, which is the whole reason the shape exists.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkClientTap* tap =
        clockwork_client_tap_open(c, CLOCKWORK_REGION_INGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 0xABCD) == CLOCKWORK_OK);

    ClockworkClientMessage msgs[8];
    const uint32_t n = clockwork_client_tap_poll(tap, msgs, 8);
    REQUIRE(n >= 1);
    CHECK(std::string(reinterpret_cast<const char*>(msgs[0].bytes)) == "/dummy/ping");
    CHECK(msgs[0].origin == 0xABCD);          // who sent it, as the ring records
    CHECK(msgs[0].length == ping.size());     // ingress frames carry no route word

    // Watching took nothing: the engine still receives it and answers.
    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    clockwork_client_tap_close(tap);
    clockwork_client_close(c);
}

TEST_CASE("client tap: two taps do not shorten each other's view",
          "[client][rings][tap]") {
    // The property the old two-handle case claimed and could not deliver.
    // Here it is real, because each tap carries its own cursor.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkClientTap* t1 = clockwork_client_tap_open(c, CLOCKWORK_REGION_INGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);
    ClockworkClientTap* t2 = clockwork_client_tap_open(c, CLOCKWORK_REGION_INGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto ping = osc_test::message("/dummy/ping");
    for (int i = 0; i < 5; ++i)
        REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 7) == CLOCKWORK_OK);

    ClockworkClientMessage m1[16], m2[16];
    const uint32_t n1 = clockwork_client_tap_poll(t1, m1, 16);
    const uint32_t n2 = clockwork_client_tap_poll(t2, m2, 16);

    CHECK(n1 == 5);
    CHECK(n2 == 5);      // draining t1 first took nothing from t2
    for (uint32_t i = 0; i < n1 && i < n2; ++i)
        CHECK(m1[i].sequence == m2[i].sequence);

    clockwork_client_tap_close(t1);
    clockwork_client_tap_close(t2);
    clockwork_client_close(c);
}

TEST_CASE("client tap: being lapped is counted, not fatal",
          "[client][rings][tap]") {
    // Writers reserve space against the ENGINE's cursor and know nothing about
    // watchers, so a tap that reads too slowly is overwritten. It must recover
    // to the newest data and say how much went by — a logger that silently
    // skipped would be worse than one that reported a gap.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkClientTap* tap = clockwork_client_tap_open(c, CLOCKWORK_REGION_INGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);
    CHECK(clockwork_client_tap_missed(tap) == 0);

    // Read one frame first, so the tap knows where it is in the sequence.
    // A gap is a distance from somewhere, and until it has read anything it
    // has nowhere to measure from.
    {
        const auto first = osc_test::message("/dummy/ping");
        REQUIRE(clockwork_client_send(c, first.ptr(), first.size(), 1) == CLOCKWORK_OK);
        ClockworkClientMessage m[4];
        REQUIRE(clockwork_client_tap_poll(tap, m, 4) == 1);
        CHECK(clockwork_client_tap_missed(tap) == 0);   // nothing missed yet
    }

    // More than the ring holds, drained by the engine as it goes so the writer
    // keeps getting room and runs right over the tap.
    const uint32_t payload = 8192;
    const auto msg = sized(payload);
    const uint32_t rounds = (IN_BUFFER_SIZE / payload) * 3;
    for (uint32_t i = 0; i < rounds; ++i) {
        while (clockwork_client_send(c, msg.data(),
                                     static_cast<uint32_t>(msg.size()), 1) == CLOCKWORK_E_FULL)
            e.pump(0.02);
        if ((i % 4) == 0) e.pump(0.02);
    }
    e.pump(0.2);

    // Whatever it finds now must be coherent frames, not rubbish read out of
    // the middle of one. The first poll after being overwritten resynchronises
    // and stops; reading on is how the tap gets going again.
    ClockworkClientMessage msgs[32];
    uint32_t frames = 0;
    for (int spin = 0; spin < 20; ++spin) {
        const uint32_t n = clockwork_client_tap_poll(tap, msgs, 32);
        for (uint32_t i = 0; i < n; ++i)
            CHECK(msgs[i].length > 0);
        frames += n;
        if (n == 0) {
            const auto again = osc_test::message("/dummy/ping");
            if (clockwork_client_send(c, again.ptr(), again.size(), 1) == CLOCKWORK_E_FULL)
                e.pump(0.02);
        }
    }
    CHECK(frames > 0);                                  // it recovered
    CHECK(clockwork_client_tap_missed(tap) > 0);        // and knows it lost some

    clockwork_client_tap_close(tap);
    clockwork_client_close(c);
}

TEST_CASE("client tap: a tap can be placed in the caller's own storage",
          "[client][rings][tap]") {
    // The same allocation-free open the handle has, for the same reason: a
    // browser client shares one heap with the engine and cannot safely call
    // into that allocator.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openClient(&st);
    REQUIRE(st == CLOCKWORK_OK);

    std::vector<uint8_t> storage(clockwork_client_tap_sizeof() + 8, 0xEE);
    ClockworkClientTap* tap = clockwork_client_tap_open_in(
        storage.data(), clockwork_client_tap_sizeof(), c,
        CLOCKWORK_REGION_INGRESS, &st);
    REQUIRE(st == CLOCKWORK_OK);
    REQUIRE(reinterpret_cast<void*>(tap) == storage.data());

    // Storage too small is refused rather than written past.
    ClockworkStatus small = CLOCKWORK_OK;
    CHECK(clockwork_client_tap_open_in(storage.data(), 4, c,
                                       CLOCKWORK_REGION_INGRESS, &small) == nullptr);
    CHECK(small == CLOCKWORK_E_ARG);

    // A region that is not a ring is not a tap.
    CHECK(clockwork_client_tap_open(c, CLOCKWORK_REGION_METRICS, &small) == nullptr);
    CHECK(small == CLOCKWORK_E_ARG);

    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 1) == CLOCKWORK_OK);
    ClockworkClientMessage m[4];
    CHECK(clockwork_client_tap_poll(tap, m, 4) == 1);

    // The guard bytes past the declared size were left alone.
    CHECK(storage[clockwork_client_tap_sizeof() + 0] == 0xEE);
    CHECK(storage[clockwork_client_tap_sizeof() + 7] == 0xEE);

    clockwork_client_tap_close(tap);
    clockwork_client_close(c);
}
