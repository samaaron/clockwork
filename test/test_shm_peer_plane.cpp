// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_shm_peer_plane.cpp — the SHM command plane (shm_peer_plane.h), in
 * process: segment geometry, the attach protocol, and the full command →
 * engine → reply-ring round trip with ShmTransport bound the way a host
 * binds it (--shm-commands).
 *
 * The cross-process crash properties (SIGKILL mid-write, lock-holding corpse,
 * reattach) are proven in test_shm_peer_crash.cpp.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "clockwork_prefix.h"
#include "OscTestUtils.h"
#include "ShmTransport.h"
#include "lanes/ring_drain.h"
#include "shm_peer_plane.h"
#include "shm_segment.hpp"
#include "workers/RingBufferWriter.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

ClockworkEngine::Config planeConfig(unsigned port) {
    ClockworkEngine::Config cfg;
    cfg.sampleRate   = 48000;
    cfg.bufferSize   = 128;
    cfg.udpPort      = port;   // non-zero enables the public shm segment
    cfg.headless     = true;
    EngineFixture::pumpByDefault(cfg);   // pumped, as defaultConfig()
    cfg.shmCommands  = true;
    return cfg;
}

// Write one OSC packet into the command ring exactly as a peer would.
bool peerWrite(ShmPeerPlaneHeader* plane, const osc_test::Packet& pkt) {
    return RingBufferWriter::write(
        shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
        &plane->cmd_head, &plane->cmd_tail,
        &plane->cmd_sequence, &plane->cmd_write_lock,
        pkt.ptr(), pkt.size(), 0);
}

// Drain every complete frame currently in the reply ring into `out`.
void drainReplies(ShmPeerPlaneHeader* plane, ClockworkDrainState& st,
                  std::vector<std::vector<uint8_t>>& out) {
    clockwork_drain_ring(
        shm_peer_rep_ring(plane), SHM_PEER_REP_RING_SIZE,
        &plane->rep_head, &plane->rep_tail, st, ClockworkDrainMetrics{}, 0,
        [&](uint32_t /*src*/, const uint8_t* d, uint32_t n, uint32_t) {
            out.emplace_back(d, d + n);
            return ClockworkDrainVerdict::Consume;
        });
}

// Pumped (planeConfig): the engine's pass drains the plane as the audio
// thread ticks, so a block a turn, and the engine answers.
bool waitUntil(ClockworkEngine& engine, const std::function<bool()>& f, int ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        engine.pumpAudioBlock();
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

TEST_CASE("shm-peer: segment publishes plane geometry and attach claims it",
          "[shm][peer]") {
    constexpr unsigned kPort = 57221;
    EngineFixture fx(planeConfig(kPort));

    shm_segment_client client(detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);

    // Geometry the peer reads (it has no compile-time view of our build).
    CHECK(plane->cmd_ring_size == SHM_PEER_CMD_RING_SIZE);
    CHECK(plane->rep_ring_size == SHM_PEER_REP_RING_SIZE);

    // Attach: generation bumps, a stale writer lock is reset, stale replies
    // are skipped (tail jumps to head).
    plane->cmd_write_lock.store(1, std::memory_order_relaxed);  // corpse held it
    uint32_t g0 = plane->generation.load(std::memory_order_relaxed);
    uint32_t g1 = shm_peer_attach(plane, 12345);
    CHECK(g1 == g0 + 1);
    CHECK(plane->owner_pid.load(std::memory_order_relaxed) == 12345);
    CHECK(plane->cmd_write_lock.load(std::memory_order_relaxed) == 0);
    CHECK(plane->rep_tail.load(std::memory_order_relaxed)
          == plane->rep_head.load(std::memory_order_relaxed));

    uint32_t g2 = shm_peer_attach(plane, 12346);  // last attach wins
    CHECK(g2 == g1 + 1);
    CHECK(plane->owner_pid.load(std::memory_order_relaxed) == 12346);
}

TEST_CASE("shm-peer: command ring commands reach the engine and reply",
          "[shm][peer]") {
    constexpr unsigned kPort = 57222;
    EngineFixture fx(planeConfig(kPort));

    shm_segment_client client(detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    // The fixture's transport surfaces replies for every token via onReply,
    // so the peer command's reply lands in the fixture collector.
    REQUIRE(peerWrite(plane, osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 4242)));

    OscReply reply;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), reply));
    CHECK(lastInt(reply) == 4242);
}

TEST_CASE("shm-peer: ShmTransport routes replies into the reply ring",
          "[shm][peer]") {
    constexpr unsigned kPort = 57223;

    // Engine wired the way a host wires --shm-commands: ShmTransport bound
    // to the engine's plane slot, set before init.
    ShmTransport     transport;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [](const uint8_t*, uint32_t) {};
    engine.setTransport(&transport);
    engine.init(planeConfig(kPort));
    transport.bindPlaneSlot(engine.peerPlaneSlot());
    REQUIRE(transport.ready());

    shm_segment_client client(detail_shm_segment::shm_dup_handle(engine.shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    ClockworkDrainState repState;
    std::vector<std::vector<uint8_t>> replies;

    // tempo/get round trip through shared memory in both directions.
    REQUIRE(peerWrite(plane, osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 7)));
    bool got = waitUntil(engine, [&] {
        drainReplies(plane, repState, replies);
        return !replies.empty();
    });
    REQUIRE(got);
    CHECK(osc_test::parseAddress(replies[0].data(),
                                 static_cast<uint32_t>(replies[0].size())) == CLOCKWORK_SYS("clock/tempo.reply"));

    // Notify subscription: /clockwork/notify from the peer flips the
    // transport's audience flag and its .reply arrives on the reply ring.
    replies.clear();
    REQUIRE_FALSE(transport.hasNotifySubscribers());
    REQUIRE(peerWrite(plane, osc_test::message("/clockwork/notify")));
    got = waitUntil(engine, [&] {
        drainReplies(plane, repState, replies);
        return !replies.empty() && transport.hasNotifySubscribers();
    });
    REQUIRE(got);

    // Reply-ring frames carry the peer's origin token in the Message header —
    // verified indirectly: send() to any other token must be undeliverable.
    CHECK_FALSE(transport.send(0, replies[0].data(),
                               static_cast<uint32_t>(replies[0].size()), false));
    CHECK(transport.send(SHM_PEER_ORIGIN_TOKEN, replies[0].data(),
                         static_cast<uint32_t>(replies[0].size()), false));

    engine.shutdown();
    CHECK_FALSE(transport.ready());  // shutdown nulls the published plane
}

TEST_CASE("shm-peer load: high-volume commands round-trip losslessly through the plane",
          "[shm][peer][load]") {
    constexpr unsigned kPort = 57226;

    // Same wiring as a host's --shm-commands, driven at volume.
    ShmTransport     transport;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [](const uint8_t*, uint32_t) {};
    engine.setTransport(&transport);
    engine.init(planeConfig(kPort));
    transport.bindPlaneSlot(engine.peerPlaneSlot());
    REQUIRE(transport.ready());

    shm_segment_client client(detail_shm_segment::shm_dup_handle(engine.shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    const PerformanceMetrics& m = engine.getMetrics();
    constexpr int N = 20000;   // medium load; both rings wrap many times over

    ClockworkDrainState repState;
    std::vector<std::vector<uint8_t>> replies;

    // Act as a real peer: keep the command ring full (the writer backpressures
    // losslessly when it's full — retry as the engine drains) while continuously
    // draining the reply ring so it never overflows (a slow peer would drop
    // replies and bump rep_dropped). Lossless end-to-end when the peer keeps up.
    int sent = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (static_cast<int>(replies.size()) < N
           && std::chrono::steady_clock::now() < deadline) {
        while (sent < N && peerWrite(plane, osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), sent + 1)))
            ++sent;
        engine.pumpAudioBlock();   // pumped: the pass drains the ring as the audio thread ticks
        drainReplies(plane, repState, replies);
        if (static_cast<int>(replies.size()) < N)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    REQUIRE(sent == N);
    REQUIRE(static_cast<int>(replies.size()) == N);
    CHECK(plane->rep_dropped.load() == 0);
    // Completeness + strict order: tempo.reply ids 1..N, monotone, none missing or
    // duplicated. Verified in plain C++ so it's one assertion, not N.
    bool ordered = true;
    for (int i = 0; i < N && ordered; ++i) {
        const auto& r = replies[i];
        const auto sz = static_cast<uint32_t>(r.size());
        const auto pr = osc_test::parseReply(r.data(), sz);
        ordered = osc_test::parseAddress(r.data(), sz) == CLOCKWORK_SYS("clock/tempo.reply")
               && pr.argCount() > 0 && pr.argInt(pr.argCount() - 1) == i + 1;
    }
    REQUIRE(ordered);
    CHECK(m.osc_in_corrupted.load(std::memory_order_relaxed) == 0);
    CHECK(plane->rep_dropped.load(std::memory_order_relaxed) == 0);  // peer kept up

    engine.shutdown();
}

TEST_CASE("shm-peer: a peer that reads its replies late loses none — the pass waits for it",
          "[shm][peer][load]") {
    // Every control verb a peer writes is answered into its reply ring, which
    // is finite; a peer that writes more than the ring holds before it reads
    // used to lose the surplus (rep_dropped), silently to the asker. The
    // control pass now leaves a verb in the control ring while the peer's
    // reply ring is nearly full, and answers it once the peer has read.
    // Pumped: the pass runs on the audio block, so the case controls when.
    constexpr unsigned kPort = 57227;

    ShmTransport     transport;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [](const uint8_t*, uint32_t) {};
    engine.setTransport(&transport);
    engine.init(planeConfig(kPort));
    transport.bindPlaneSlot(engine.peerPlaneSlot());
    REQUIRE(transport.ready());

    shm_segment_client client(detail_shm_segment::shm_dup_handle(engine.shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    // More verbs than the reply ring can hold answers for, written before a
    // single reply is read.
    constexpr int N = 6000;
    int sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (sent < N && std::chrono::steady_clock::now() < deadline) {
        while (sent < N && peerWrite(plane, osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), sent + 1)))
            ++sent;
        for (int i = 0; i < 8; ++i) engine.pumpAudioBlock();   // the pass runs; the reply ring fills; the rest waits
    }
    REQUIRE(sent == N);
    for (int i = 0; i < 50; ++i) engine.pumpAudioBlock();
    CHECK(plane->rep_dropped.load() == 0);

    // Read, and the rest is answered: every one, in order.
    ClockworkDrainState repState;
    std::vector<std::vector<uint8_t>> replies;
    const auto readBy = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (static_cast<int>(replies.size()) < N && std::chrono::steady_clock::now() < readBy) {
        drainReplies(plane, repState, replies);
        engine.pumpAudioBlock();
    }
    REQUIRE(static_cast<int>(replies.size()) == N);
    CHECK(plane->rep_dropped.load() == 0);
    engine.shutdown();
}

TEST_CASE("shm-peer: answers a full reply ring refuses are counted undelivered, in the engine's metrics as well as the plane",
          "[shm][peer][egress]") {
    // The audio thread answers a peer's /clockwork/sync in its block, and the
    // gateway hands each answer to the peer's reply ring, which is finite. A
    // peer that asks more than the ring holds answers for before it reads
    // loses the rest: counted in its plane (rep_dropped), for the peer, and in
    // the engine's metrics (egress_frames_undelivered), for whoever reads
    // them. Pumped, and no pass but the case's own: a block, then a pass that
    // takes the peer's commands and hands the answers to its ring.
    constexpr unsigned kPort = 57229;

    ShmTransport     transport;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [](const uint8_t*, uint32_t) {};
    engine.setTransport(&transport);
    auto cfg = planeConfig(kPort);
    cfg.hostDrivesControl = true;
    engine.init(cfg);
    transport.bindPlaneSlot(engine.peerPlaneSlot());
    REQUIRE(transport.ready());

    shm_segment_client client(detail_shm_segment::shm_dup_handle(engine.shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    auto block = [&] { engine.pumpAudioBlock(); REQUIRE(engine.controlPass()); };
    constexpr int N = 6000;
    int sent = 0;
    for (int round = 0; sent < N && round < 1000; ++round) {
        while (sent < N && peerWrite(plane, osc_test::message(CLOCKWORK_SYS("sync"), sent + 1)))
            ++sent;
        for (int i = 0; i < 8; ++i) block();
    }
    REQUIRE(sent == N);
    for (int i = 0; i < N / 32 + 50; ++i) block();

    const uint32_t lost = plane->rep_dropped.load();
    const auto& m = engine.getMetrics();
    INFO("cmd ring head " << plane->cmd_head.load() << " tail " << plane->cmd_tail.load()
         << "; taken " << m.osc_out_messages_sent.load() << ", dropped " << m.messages_dropped.load());
    INFO(m.messages_processed.load() << " processed, " << m.egress_frames_refused.load() << " refused, "
         << lost << " lost at the reply ring, " << m.egress_frames_undelivered.load() << " counted undelivered");
    REQUIRE(lost > 0);
    CHECK(m.egress_frames_undelivered.load() == lost);
    engine.shutdown();
}
