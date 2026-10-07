// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_egress.cpp — the client reads the engine's egress itself.
 *
 * The engine writes two rings: the audio thread's replies (OUT) and the
 * NRT gateway's (NRT-out — the subsystems, the state changes, the
 * broadcasts). One read function, clockwork_client_poll, takes from both,
 * each frame with the origin it answers and the route it was sent by. A
 * host that drains egress itself (Config::hostDrainsEgress) tells the engine
 * so, and the engine's own gateway then leaves the rings alone: the host is
 * the consumer, on native as the JavaScript client already is on the web.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "shared_memory.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// The engine's arena, as audio_processor.cpp publishes it, and the OUT ring's
// writer: the audio thread's, driven by hand below so the ring fills exactly
// when the case says so.
extern "C" { extern uint8_t* shared_memory; }
extern bool ring_buffer_write(
    uint8_t* buffer_start, uint32_t buffer_size,
    std::atomic<int32_t>* head, std::atomic<int32_t>* tail, std::atomic<int32_t>* sequence,
    uint32_t route, uint32_t source_id, const void* data, uint32_t data_size,
    std::atomic<uint32_t>* status_flags, PerformanceMetrics* metrics);

namespace {

struct Frame { uint32_t origin, route; std::string address; };

// Everything the client can take right now.
std::vector<Frame> takeAll(ClockworkClient* c) {
    std::vector<Frame> out;
    ClockworkClientMessage batch[64];
    for (;;) {
        const uint32_t n = clockwork_client_poll(c, batch, 64);
        for (uint32_t i = 0; i < n; ++i)
            out.push_back({ batch[i].origin, batch[i].route, osc_test::parseAddress(batch[i].bytes, batch[i].length) });
        if (n < 64) break;
    }
    return out;
}

// Polls until a frame at `address` arrives, or gives up.
bool waitForFrame(ClockworkClient* c, const char* address, Frame* out, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const Frame& f : takeAll(c))
            if (f.address == address) { if (out) *out = f; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("client egress: one poll takes from both rings, each frame with its origin and route",
          "[client][egress]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    ClockworkClient* c = fx.engine().egressClient();
    REQUIRE(c != nullptr);

    // Boot wrote to the NRT ring before anyone polled: the state changes are
    // still there, routed to the notify audience, and the client takes them.
    bool sawNotify = false;
    for (const Frame& f : takeAll(c)) if (f.route == EGRESS_BROADCAST_NOTIFY) sawNotify = true;
    CHECK(sawNotify);

    // The audio thread answers on the OUT ring, to the token that asked.
    const auto ping = osc_test::message("/dummy/ping");
    fx.engine().ingest(ping.ptr(), ping.size(), 7);
    Frame pong;
    REQUIRE(waitForFrame(c, "/dummy/pong", &pong));
    CHECK(pong.origin == 7);
    CHECK(pong.route == EGRESS_REPLY);

    // The NRT gateway answers on the NRT-out ring, to its token, and the
    // same poll takes that too.
    const auto notify = osc_test::message("/clockwork/notify", int32_t{1});
    fx.engine().ingest(notify.ptr(), notify.size(), 9);
    Frame reply;
    REQUIRE(waitForFrame(c, "/clockwork/notify.reply", &reply));
    CHECK(reply.origin == 9);
    CHECK(reply.route == EGRESS_REPLY);
}

TEST_CASE("client egress: with the host draining, the engine's gateway leaves the rings alone",
          "[client][egress]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    // The fixture listens through the engine's transport, which the gateway
    // used to feed. Nothing reaches it now: the frames wait in the rings for
    // the host.
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    CHECK_FALSE(fx.waitForReply("/dummy/pong", r, 400));
    Frame pong;
    CHECK(waitForFrame(fx.engine().egressClient(), "/dummy/pong", &pong));
}

TEST_CASE("client egress: without it, the engine delivers as it always did", "[client][egress]") {
    EngineFixture fx;   // hostDrainsEgress false
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    CHECK(fx.waitForReply("/dummy/pong", r));
}

TEST_CASE("client egress: a polled message is the caller's until the next poll, however full the ring gets meanwhile",
          "[client][egress]") {
    // WHAT A POINTER INTO THE RING PROMISES. Poll hands back the bytes where
    // they lie rather than a copy, and every caller — the engine's own egress
    // thread, the Rust client, the worklet — reads them after poll returns.
    // That is only sound if the space stays the caller's until it polls
    // again: a tail given back to the writer at poll time lets the audio
    // thread lap the ring and write a new reply over the one still being read.
    // Seen once under TSan on a loaded CI runner (2026-10-07), never locally:
    // the window is microseconds wide, so the ring is filled by hand here.
    engine_test::Engine e([](ClockworkEngine::Config& cfg) { cfg.hostDrainsEgress = true; });
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = clockwork_client_open_memory(shared_memory, TOTAL_BUFFER_SIZE, &st);
    REQUIRE(st == CLOCKWORK_OK);
    takeAll(c);   // boot's broadcasts

    // Put the reply half-way round the ring, where the writer will wrap on to
    // it: at the ring's start the front has no room for a frame and the
    // writer refuses before it ever reaches the held bytes.
    auto* ctl = reinterpret_cast<ControlPointers*>(shared_memory + CONTROL_START);
    const auto fill = [&](const std::vector<uint8_t>& payload) {
        return ring_buffer_write(shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                                 &ctl->out_head, &ctl->out_tail, &ctl->out_sequence,
                                 EGRESS_REPLY, 7, payload.data(), static_cast<uint32_t>(payload.size()),
                                 nullptr, nullptr);
    };
    const std::vector<uint8_t> coarse(200, 0xEE);
    while (static_cast<uint32_t>(ctl->out_head.load()) < OUT_BUFFER_SIZE / 2) REQUIRE(fill(coarse));
    takeAll(c);

    // One reply on the OUT ring, taken and held. Written by hand, and long,
    // so that a lap cannot stop short of its bytes by the luck of a frame
    // boundary: whatever ends up written last ends within a frame of the
    // tail, and this payload reaches two hundred bytes back from it.
    const std::vector<uint8_t> mark(200, 0xAB);
    REQUIRE(fill(mark));
    ClockworkClientMessage batch[64];
    REQUIRE(clockwork_client_poll(c, batch, 64) == 1);
    const ClockworkClientMessage held = batch[0];
    REQUIRE(held.length == mark.size());
    CHECK(std::vector<uint8_t>(held.bytes, held.bytes + held.length) == mark);

    // The audio thread writes replies behind it until the ring is full, which
    // takes it past the end and round to the front. In frames smaller than the
    // held one, so the writer packs right up to whatever it takes the tail to
    // be: short of the held bytes, or into them.
    const int32_t headBefore = ctl->out_head.load();
    const std::vector<uint8_t> fine(8, 0xEE);
    uint32_t written = 0;
    while (fill(fine)) {
        ++written;
        REQUIRE(written < 100000);   // a ring that never fills is a failure, not a hang
    }
    CHECK(written > 0);
    CHECK(ctl->out_head.load() < headBefore);   // it came round

    // The held bytes are untouched: the writer stopped short of them.
    CHECK(std::vector<uint8_t>(held.bytes, held.bytes + held.length) == mark);

    // And the ring goes on working: the next poll takes the filler, which
    // gives the space back, and a fresh ping is answered.
    uint32_t taken = 0;
    for (;;) {
        const uint32_t m = clockwork_client_poll(c, batch, 64);
        taken += m;
        if (m < 64) break;
    }
    CHECK(taken == written);
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(clockwork_client_send(c, ping.ptr(), ping.size(), 7) == CLOCKWORK_OK);
    e.pump(0.1);
    Frame pong;
    CHECK(waitForFrame(c, "/dummy/pong", &pong, 200));
    clockwork_client_close(c);
}
