// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_host_control.cpp — the host runs the engine's control plane.
 *
 * Every /clockwork/ verb the audio thread does not answer itself is forwarded
 * to the control ring, and ONE pass — the control-ring drain, the peer
 * command plane, the MIDI clock producer, the egress drain — answers it off
 * the audio thread. Whose thread runs that pass is the host's choice:
 * the engine's own gateway thread, woken every audio block, or the host's,
 * calling ClockworkEngine::controlPass() (Config::hostDrivesControl). This
 * file pins the second shape: the engine starts no thread, nothing is
 * answered until the host passes, every subsystem is reached through the
 * host's thread, and the door closes cleanly at shutdown.
 *
 * The whole engine suite can also be run with every fixture in this shape
 * (CLOCKWORK_TEST_HOST_DRIVES_CONTROL=1, see EngineFixture.h); these cases
 * are the ones that say what the shape IS.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "clockwork_prefix.h"
#include "shared_memory.h"
#include "shm_peer_plane.h"
#include "shm_segment.hpp"
#include "workers/RingBufferWriter.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using engine_test::Engine;

// The addresses of every reply collected so far.
std::vector<std::string> addresses(Engine& e) {
    std::lock_guard<std::mutex> lock(e.mu);
    std::vector<std::string> out;
    for (const auto& r : e.replies)
        out.push_back(osc_test::parseAddress(r.data(), static_cast<uint32_t>(r.size())));
    return out;
}

bool has(const std::vector<std::string>& v, const char* addr) {
    for (const auto& s : v) if (s == addr) return true;
    return false;
}

// Passes on the calling thread until `addr` has been answered, or gives up.
// A reply framed during one pass is taken by the next (the egress drain runs
// first in the pass), so one pass is never enough and a bound is needed.
bool passUntil(Engine& e, const char* addr, int maxPasses = 50) {
    for (int i = 0; i < maxPasses; ++i) {
        if (has(addresses(e), addr)) return true;
        if (!e.engine.controlPass()) return false;
    }
    return has(addresses(e), addr);
}

ClockworkEngine::Config hostDriven(ClockworkEngine::Config cfg) {
    cfg.hostDrivesControl = true;
    return cfg;
}

} // namespace

TEST_CASE("host control: the engine starts no thread, and nothing is answered until the host passes",
          "[host][control]") {
    Engine e([](ClockworkEngine::Config& cfg) { cfg.hostDrivesControl = true; });
    // The fixture's own host thread is what a host would run; here the TEST
    // is the host, so it is stopped and the passes are made by hand.
    e.hostControl.stop();

    CHECK_FALSE(e.engine.hasControlThread());

    // One verb for each side of the boundary's audio-thread half: /dummy/ping
    // is answered on the audio thread (onto the OUT ring), /clockwork/notify
    // is forwarded to the control ring. Both are pumped through the audio
    // thread; neither reaches the transport, because nothing drains.
    const auto ping   = osc_test::message("/dummy/ping");
    const auto notify = osc_test::message(CLOCKWORK_SYS("notify"), int32_t{1});
    e.engine.ingest(ping.ptr(), ping.size(), 7);
    e.engine.ingest(notify.ptr(), notify.size(), 9);
    e.pump(0.05);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(addresses(e).empty());

    // The host passes: the pong comes off OUT, the notify command comes off
    // the control ring and is answered onto NRT-out, and the pass after that
    // takes the answer.
    REQUIRE(e.engine.controlPass());
    REQUIRE(passUntil(e, "/dummy/pong"));
    REQUIRE(passUntil(e, CLOCKWORK_SYS("notify.reply")));
}

TEST_CASE("host control: the pass is refused before init, after shutdown, and beside the engine's own thread",
          "[host][control]") {
    ClockworkEngine cold;
    CHECK_FALSE(cold.controlPass());
    CHECK_FALSE(cold.hasControlThread());

    {
        // The engine runs its own gateway thread: a host's pass would be a
        // second consumer on the same rings, so the door stays shut.
        Engine e([](ClockworkEngine::Config& cfg) { cfg.hostDrivesControl = false; });
        CHECK(e.engine.hasControlThread());
        CHECK_FALSE(e.engine.controlPass());
    }

    {
        Engine e([](ClockworkEngine::Config& cfg) { cfg.hostDrivesControl = true; });
        e.hostControl.stop();
        CHECK(e.engine.controlPass());
        e.engine.shutdown();
        // The door is closed: a host that keeps calling gets false, and the
        // subsystems the pass would have reached are gone without incident.
        CHECK_FALSE(e.engine.controlPass());
        CHECK_FALSE(e.engine.hasControlThread());
        // (The fixture's destructor shuts down again; shutdown is idempotent.)
    }
}

TEST_CASE("host control: every subsystem answers through the host's thread",
          "[host][control]") {
    EngineFixture fx(hostDriven(EngineFixture::defaultConfig()));
    REQUIRE(fx.hostDrivesControl());
    CHECK_FALSE(fx.engine().hasControlThread());

    OscReply r;

    // The engine's own verbs (the control table's fallback).
    fx.send(osc_test::message(CLOCKWORK_SYS("notify"), int32_t{1}));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("notify.reply"), r));

    // "clock/": the Link surface, with its correlation token echoed last.
    fx.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{4242}));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r));
    CHECK(lastInt(r) == 4242);

#ifdef CLOCKWORK_MIDI
    // "midi/": the Rust subsystem, reached synchronously from the pass.
    fx.send(osc_test::message(CLOCKWORK_SYS("midi/ports/list")));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("midi/ports.reply"), r));
#endif
#ifdef CLOCKWORK_OSC
    fx.send(osc_test::message(CLOCKWORK_SYS("osc/notify/subscribe"), int32_t{31}));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("osc/notify/subscribe.reply"), r));
    CHECK(lastInt(r) == 31);
#endif
#ifdef CLOCKWORK_GAMEPAD
    fx.send(osc_test::message(CLOCKWORK_SYS("gamepad/notify/subscribe"), int32_t{32}));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("gamepad/notify/subscribe.reply"), r));
    CHECK(lastInt(r) == 32);
#endif

    // An address under the prefix nobody owns is refused at the far end of
    // the pass, as it is at the far end of the gateway thread.
    fx.send(osc_test::message(CLOCKWORK_SYS("no/such/verb")));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("error"), r));
}

TEST_CASE("host control: the audio-thread verbs never wait for the host",
          "[host][control]") {
    // ping and echo are the liveness surface: answered on the audio thread
    // without a hop. With the host driving control they are still answered
    // there — only the DELIVERY of the answer waits for a pass, as it always
    // waited for the gateway.
    Engine e([](ClockworkEngine::Config& cfg) { cfg.hostDrivesControl = true; });
    e.hostControl.stop();

    const auto ping = osc_test::message(CLOCKWORK_SYS("ping"), int32_t{77});
    e.engine.ingest(ping.ptr(), ping.size(), 3);
    e.pump(0.01);
    REQUIRE(passUntil(e, CLOCKWORK_SYS("pong")));
}

TEST_CASE("host control: with the host draining egress too, no engine thread stands between a command and its reply",
          "[host][control][egress]") {
    ClockworkEngine::Config cfg = hostDriven(EngineFixture::defaultConfig());
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    CHECK_FALSE(fx.engine().hasControlThread());
    ClockworkClient* c = fx.engine().egressClient();
    REQUIRE(c != nullptr);

    // The host's pass answers the command; the host's poll takes the answer.
    // The engine's transport sees nothing.
    const auto notify = osc_test::message(CLOCKWORK_SYS("notify"), int32_t{1});
    fx.engine().ingest(notify.ptr(), notify.size(), 9);

    bool found = false;
    uint32_t origin = 0, route = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!found && std::chrono::steady_clock::now() < deadline) {
        ClockworkClientMessage batch[64];
        const uint32_t n = clockwork_client_poll(c, batch, 64);
        for (uint32_t i = 0; i < n && !found; ++i) {
            if (osc_test::parseAddress(batch[i].bytes, batch[i].length) == CLOCKWORK_SYS("notify.reply")) {
                found  = true;
                origin = batch[i].origin;
                route  = batch[i].route;
            }
        }
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(found);
    CHECK(origin == 9);
    CHECK(route == EGRESS_REPLY);

    OscReply r;
    CHECK_FALSE(fx.waitForReply(CLOCKWORK_SYS("notify.reply"), r, 200));
}

TEST_CASE("host control: the peer command plane rides the host's pass",
          "[host][control][shm][peer]") {
    constexpr unsigned kPort = 57231;
    ClockworkEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.udpPort           = kPort;   // non-zero creates the public segment
    cfg.headless          = true;
    cfg.shmCommands       = true;
    cfg.hostDrivesControl = true;
    EngineFixture fx(cfg);
    CHECK_FALSE(fx.engine().hasControlThread());

    shm_segment_client client(detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle()));
    ShmPeerPlaneHeader* plane = client.get_peer_plane();
    REQUIRE(plane != nullptr);
    shm_peer_attach(plane, 1);

    // Exactly as a peer writes: into the plane's command ring. The host's
    // pass feeds it to ingest, the audio thread forwards it, the next pass
    // answers it.
    const auto get = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{4343});
    REQUIRE(RingBufferWriter::write(
        shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
        &plane->cmd_head, &plane->cmd_tail,
        &plane->cmd_sequence, &plane->cmd_write_lock,
        get.ptr(), get.size(), 0));

    OscReply reply;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), reply));
    CHECK(lastInt(reply) == 4343);
}
