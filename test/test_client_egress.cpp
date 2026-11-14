// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_egress.cpp — the client reads the engine's egress itself.
 *
 * The engine writes two rings: the audio thread's replies (OUT) and the
 * NRT gateway's (NRT-out — the subsystems, the state changes, the
 * broadcasts). One read function, clockwork_client_poll, takes from both,
 * each frame with the origin it answers. A
 * host that drains egress itself (Config::hostDrainsEgress) tells the engine
 * so, and the engine's own gateway then leaves the rings alone: the host is
 * the consumer, on native as the JavaScript client already is on the web.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "lanes/lanes.h"
#include <functional>
#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "clockwork_prefix.h"
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
namespace {
// The engine's arena (clockwork_lanes_base).
uint8_t* arenaBase(const engine_test::Engine& e) { return static_cast<uint8_t*>(clockwork_lanes_base(e.engine.core())); }
}
extern bool ring_buffer_write(
    uint8_t* buffer_start, uint32_t buffer_size,
    std::atomic<int32_t>* head, std::atomic<int32_t>* tail, std::atomic<int32_t>* sequence,
    uint32_t source_id, const void* data, uint32_t data_size,
    std::atomic<uint32_t>* status_flags, PerformanceMetrics* metrics);

namespace {

struct Frame { uint32_t origin; std::string address; };

// Everything the client can take right now.
std::vector<Frame> takeAll(ClockworkClient* c) {
    std::vector<Frame> out;
    ClockworkClientMessage batch[64];
    for (;;) {
        const uint32_t n = clockwork_client_poll(c, batch, 64);
        for (uint32_t i = 0; i < n; ++i)
            out.push_back({ batch[i].origin, osc_test::parseAddress(batch[i].bytes, batch[i].length) });
        if (n < 64) break;
    }
    return out;
}

// Polls until a frame at `address` arrives, or gives up; `pump` renders a
// block per poll, since nothing else renders what the audio thread answers.
bool waitForFrame(const std::function<void()>& pump, ClockworkClient* c, const char* address, Frame* out, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        pump();
        for (const Frame& f : takeAll(c))
            if (f.address == address) { if (out) *out = f; return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("client egress: one poll takes from both rings, each frame with its origin",
          "[client][egress]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    ClockworkClient* c = fx.engine().egressClient();
    REQUIRE(c != nullptr);

    takeAll(c);   // what boot said, set aside

    // The audio thread answers on the OUT ring, to the token that asked.
    const auto ping = osc_test::message("/dummy/ping");
    fx.engine().ingest(ping.ptr(), ping.size(), 7);
    Frame pong;
    REQUIRE(waitForFrame([&] { fx.pumpBlock(); }, c, "/dummy/pong", &pong));
    CHECK(pong.origin == 7);

    // The NRT gateway answers on the NRT-out ring, to its token, and the
    // same poll takes that too.
    const auto notify = osc_test::message("/clockwork/notify", int32_t{1});
    fx.engine().ingest(notify.ptr(), notify.size(), 9);
    Frame reply;
    REQUIRE(waitForFrame([&] { fx.pumpBlock(); }, c, "/clockwork/notify.reply", &reply));
    CHECK(reply.origin == 9);
}

TEST_CASE("client egress: with the host draining, the engine's gateway leaves the rings alone",
          "[client][egress]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    // The fixture listens through the engine's transport. With the host
    // draining, the gateway does not feed it: the frames wait in the rings
    // for the host.
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    CHECK_FALSE(fx.waitForReply("/dummy/pong", r, 400));
    Frame pong;
    CHECK(waitForFrame([&] { fx.pumpBlock(); }, fx.engine().egressClient(), "/dummy/pong", &pong));
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
    // The window is microseconds wide, so the ring is filled by hand here.
    engine_test::Engine e([](ClockworkEngine::Config& cfg) { cfg.hostDrainsEgress = true; });
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = clockwork_client_open_memory(arenaBase(e), clockwork::kDefaultLayout.arena_bytes, &st);
    REQUIRE(st == CLOCKWORK_OK);
    takeAll(c);   // boot's broadcasts

    // Put the reply half-way round the ring, where the writer will wrap on to
    // it: at the ring's start the front has no room for a frame and the
    // writer refuses before it ever reaches the held bytes.
    auto* ctl = reinterpret_cast<RingCursors*>(arenaBase(e) + clockwork::kDefaultLayout.ring_cursors_start);
    const auto fill = [&](const std::vector<uint8_t>& payload) {
        return ring_buffer_write(arenaBase(e) + clockwork::kDefaultLayout.rt_out_ring_start, clockwork::kDefaultLayout.profile.rt_out_ring_bytes,
                                 &ctl->out_head, &ctl->out_tail, &ctl->out_sequence,
                                 7, payload.data(), static_cast<uint32_t>(payload.size()),
                                 nullptr, nullptr);
    };
    const std::vector<uint8_t> coarse(200, 0xEE);
    while (static_cast<uint32_t>(ctl->out_head.load()) < clockwork::kDefaultLayout.profile.rt_out_ring_bytes / 2) REQUIRE(fill(coarse));
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
    CHECK(waitForFrame([&] { e.engine.pumpAudioBlock(); }, c, "/dummy/pong", &pong, 200));
    clockwork_client_close(c);
}

// A host whose screen runs on a loop it does not own parks a thread in
// clockwork_client_wait and has it tell the loop, rather than poll on a timer.
// The waits below have deadlines only so that a wake that never comes fails
// the case rather than hanging it.
TEST_CASE("client egress: a thread waits until the engine says something, then polls what it said",
          "[client][egress][wait]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    ClockworkClient* c = fx.engine().egressClient();
    REQUIRE(c != nullptr);

    // Boot has spoken, so the first wait is over at once: a host that waits
    // before it first polls misses nothing.
    uint32_t seen = 0;
    REQUIRE(clockwork_client_wait(c, &seen, 0) == CLOCKWORK_OK);
    CHECK(seen != 0);
    takeAll(c);

    // Nothing new since: a look says so.
    uint32_t still = seen;
    CHECK(clockwork_client_wait(c, &still, 0) == CLOCKWORK_E_TIMEOUT);
    CHECK(still == seen);

    // A thread parks; the reply to a ping wakes it, and is there to take.
    std::atomic<bool> parked{false};
    bool timedOut = false, answered = false;
    std::thread waiter([&] {
        uint32_t mine = seen;
        parked = true;
        while (!answered && !timedOut) {
            if (clockwork_client_wait(c, &mine, 3000) == CLOCKWORK_E_TIMEOUT) timedOut = true;
            for (const Frame& f : takeAll(c)) if (f.address == "/dummy/pong") answered = true;
        }
    });
    while (!parked) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // asleep, not merely started
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(fx.engine().ingest(ping.ptr(), ping.size(), 7) == CLOCKWORK_OK);
    fx.pumpBlock();
    waiter.join();
    CHECK(answered);
    CHECK_FALSE(timedOut);
}

TEST_CASE("client egress: a wake ends a wait with no deadline, from another thread", "[client][egress][wait]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    ClockworkClient* c = fx.engine().egressClient();
    uint32_t seen = 0;
    clockwork_client_wait(c, &seen, 0);

    std::atomic<ClockworkStatus> got{CLOCKWORK_E_ARG};
    const auto start = std::chrono::steady_clock::now();
    std::thread waiter([&] {
        uint32_t mine = seen;
        // Long enough that only a wake (or the engine speaking) ends it in time.
        got = clockwork_client_wait(c, &mine, 10000);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    clockwork_client_wake(c);
    waiter.join();
    CHECK(got.load() == CLOCKWORK_OK);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));

    // And what a wait refuses.
    CHECK(clockwork_client_wait(nullptr, &seen, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_client_wait(c, nullptr, 0) == CLOCKWORK_E_ARG);
    CHECK(std::string(clockwork_client_status_text(CLOCKWORK_E_TIMEOUT)) != "unknown status");
}

// The control pass holds a verb back while the egress ring lacks room for its
// answers. A host that drains egress itself makes that room, and the engine
// answers the rest: the host's read wakes it, with no further write and no
// audio block.
TEST_CASE("client egress: verbs held for room in the egress ring are answered once the host has read, with no audio block",
          "[client][egress][wake]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    if (!cfg.manualAudioPump || cfg.hostDrivesControl)
        SKIP("needs the engine's own control pass and no audio thread");
    cfg.hostDrainsEgress = true;
    EngineFixture fx(cfg);
    ClockworkClient* host = fx.engine().egressClient();
    takeAll(host);   // boot's broadcasts

    // A time verb's answer is larger than the verb: a full control ring of
    // them answers with more than the egress ring holds.
    const auto ask = osc_test::message(CLOCKWORK_SYS("clock/time/now/get"));
    int sent = 0;
    while (sent < 100000 && fx.engine().ingest(ask.ptr(), ask.size(), 0) == CLOCKWORK_OK) ++sent;
    REQUIRE(sent > 0);

    // Read, and only read.
    int got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got < sent && std::chrono::steady_clock::now() < deadline) {
        for (const Frame& f : takeAll(host))
            if (f.address == CLOCKWORK_SYS("clock/time/now.reply")) ++got;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(got == sent);
}
