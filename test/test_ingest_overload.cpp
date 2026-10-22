// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_ingest_overload.cpp — a client that sends more than the engine can take
 * can tell that it did.
 *
 * The engine takes commands through its command ring, which drains a block at
 * a time. A client that outruns it has the commands that do not fit dropped:
 * the client's error, not a fault, but never a silent one. Every command a
 * client sends is either taken or counted dropped in the metrics it reads
 * (clockwork_client_metrics), and every command taken is answered or its
 * answer counted refused, so a client can see that it overloaded the engine
 * and by how much. An answer the egress ring has no room for is dropped, as
 * a command the command ring has no room for is: the audio thread never waits
 * for a client to read. SuperSonic 0.90.1's
 * publish was stopped by a transport test that did just that: its TCP load
 * sent 5,000 commands at once, a loaded Windows runner's engine fell behind,
 * and the probe waited for replies to commands that had been dropped.
 *
 * Unix-only: the client side uses POSIX sockets directly.
 */
#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "StreamTestClient.h"
#include "clockwork_client.h"
#include "clockwork_prefix.h"
#include "comms/StreamOscTransport.h"

#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace {

// The whole of a buffer down a socket, from a thread that is not Catch's (no
// REQUIRE), and without a SIGPIPE if the server goes: false if it went.
bool sendAll(int fd, const std::vector<uint8_t>& bytes) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, kFlags);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> framed(const std::vector<uint8_t>& pkt) {
    std::vector<uint8_t> out(4 + pkt.size());
    const uint32_t n = static_cast<uint32_t>(pkt.size());
    out[0] = uint8_t(n >> 24);
    out[1] = uint8_t(n >> 16);
    out[2] = uint8_t(n >> 8);
    out[3] = uint8_t(n);
    std::copy(pkt.begin(), pkt.end(), out.begin() + 4);
    return out;
}

// The counters a client reads to know what became of its commands, read as a
// block, as the metrics' contract asks: the commands the engine took
// (osc_out_messages_sent, the client's view of the host→engine direction),
// those it dropped (messages_dropped), those it processed off the ring
// (messages_processed), the answers an egress ring refused
// (egress_frames_refused), and those the transport could not deliver
// (egress_frames_undelivered).
struct Counts {
    uint32_t taken = 0, dropped = 0, processed = 0, refused = 0, undelivered = 0;
};
Counts readCounts(ClockworkEngine& engine) {
    std::array<uint32_t, 64> m{};
    REQUIRE(clockwork_client_metrics(engine.egressClient(), m.data(), uint32_t(m.size())) >= 63);
    return {m[9], m[2], m[1], m[55], m[62]};
}

bool waitFor(const std::function<bool()>& f) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

}  // namespace

// Far more commands than the engine can take, sent over TCP as fast as
// loopback carries them: the engine drops what does not fit, and the client
// sees it did, in the metrics; and between them, what was taken (every one
// answered, or its answer refused) and what was dropped account for every
// command sent.
TEST_CASE("A client that overloads the engine sees it in the metrics and no command goes unaccounted",
          "[transport][tcp][overload][rt]") {
    constexpr int kCommands = 200000;

    StreamOscTransport tcp;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    tcp.setIngest([&engine](const uint8_t* d, uint32_t n, uint32_t token) {
        engine.ingest(d, n, token);
    });
    tcp.initialiseTcp(0, "127.0.0.1");
    engine.setTransport(&tcp);
    auto cfg = EngineFixture::driverConfig();   // the engine draining under load on its own: nobody here to pump it
    cfg.hostDrivesControl = false;   // the engine runs its own control pass
    engine.init(cfg);
    REQUIRE(tcp.start());
    const Counts before = readCounts(engine);

    const int fd = connectTcp(tcp.boundPort());
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    std::atomic<int> sent{0}, answered{0}, outOfOrder{0};
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        for (int id = 1; id <= kCommands; ++id) {
            if (!sendAll(fd, framed(osc_test::message(CLOCKWORK_SYS("sync"), id).data))) return;
            sent.store(id);
        }
    });
    std::thread reader([&] {
        int last = 0;
        while (!finished.load()) {
            const auto frame = readFramed(fd);   // empty after 2 s of nothing, or at the end
            if (frame.empty()) continue;
            const auto reply = osc_test::parseReply(frame.data(), static_cast<uint32_t>(frame.size()));
            if (reply.address != CLOCKWORK_SYS("synced")) continue;
            const int id = reply.argInt(0);
            if (id <= last) outOfOrder.fetch_add(1);
            last = id;
            answered.fetch_add(1);
        }
    });

    // Every command has been taken or dropped, and every one taken processed
    // and answered or its answer refused.
    writer.join();
    Counts now{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        now = readCounts(engine);
    } while ((now.taken - before.taken + now.dropped - before.dropped < uint32_t(kCommands)
              || now.processed - before.processed < now.taken - before.taken
              || answered.load() + int(now.refused - before.refused) < int(now.taken - before.taken))
             && std::chrono::steady_clock::now() < deadline);
    finished.store(true);
    shutdown(fd, SHUT_RDWR);
    reader.join();
    close(fd);
    engine.shutdown();
    tcp.stop();

    const uint32_t taken = now.taken - before.taken;
    const uint32_t dropped = now.dropped - before.dropped;
    const uint32_t processed = now.processed - before.processed;
    const uint32_t refused = now.refused - before.refused;
    INFO(sent.load() << " sent: " << taken << " taken, " << processed << " processed, "
         << answered.load() << " answered, " << refused << " answers refused, " << dropped << " dropped");
    CHECK(sent.load() == kCommands);
    // Overloaded, and the client can see it: n sent, m dropped and counted,
    // n - m taken and processed, each answered or its answer refused and
    // counted.
    CHECK(dropped > 0);
    CHECK(taken + dropped == uint32_t(kCommands));
    CHECK(processed == uint32_t(kCommands) - dropped);
    CHECK(answered.load() + int(refused) == kCommands - int(dropped));
    CHECK(outOfOrder.load() == 0);
}

// A client that closes its connection before its answers come back gets none
// of them: the transport has nowhere to send them. They left the egress ring
// and reached no one, and the metrics say so, one per answer.
TEST_CASE("Answers to a client that has gone are counted undelivered",
          "[transport][tcp][egress]") {
    StreamOscTransport tcp;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    tcp.setIngest([&engine](const uint8_t* d, uint32_t n, uint32_t token) {
        engine.ingest(d, n, token);
    });
    tcp.initialiseTcp(0, "127.0.0.1");
    engine.setTransport(&tcp);
    auto cfg = EngineFixture::defaultConfig();   // pumped: no block but this case's
    cfg.hostDrivesControl = true;                 // and no pass but this case's
    engine.init(cfg);
    REQUIRE(tcp.start());

    // Subscribed, so the transport's audience says when it has seen the
    // connection close.
    const int fd = connectTcp(tcp.boundPort());
    REQUIRE(sendAll(fd, framed(osc_test::message(CLOCKWORK_SYS("notify")).data)));
    REQUIRE(waitFor([&] { engine.controlPass(); return tcp.hasNotifySubscribers(); }));
    REQUIRE(engine.controlPass());   // the subscription's answer, delivered

    const Counts before = readCounts(engine);
    constexpr uint32_t kAsked = 5;
    for (uint32_t id = 1; id <= kAsked; ++id)
        REQUIRE(sendAll(fd, framed(osc_test::message(CLOCKWORK_SYS("sync"), int(id)).data)));
    REQUIRE(waitFor([&] { return readCounts(engine).taken - before.taken == kAsked; }));
    close(fd);
    REQUIRE(waitFor([&] { return !tcp.hasNotifySubscribers(); }));

    engine.pumpAudioBlock();          // answered, into the egress ring
    REQUIRE(engine.controlPass());    // and handed to a transport with no one to give them to
    const Counts after = readCounts(engine);
    INFO((after.processed - before.processed) << " answered by the engine, "
         << (after.undelivered - before.undelivered) << " counted undelivered");
    CHECK(after.processed - before.processed == kAsked);
    CHECK(after.undelivered - before.undelivered == kAsked);
    engine.shutdown();
    tcp.stop();
}

#endif  // _WIN32
