// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_ring_concurrency.cpp — true multi-thread coverage for clockwork's
 * ingress/egress rings.
 *
 * Contract pinned here: the rings' threading assumptions. The rest of the ring
 * suite (test_ring_buffer_write, test_lanes) is thorough but single-threaded —
 * it proves framing, wraparound, corruption repair and overflow accounting,
 * never real producer/consumer contention. This file closes that gap and,
 * under a TSan build, turns each ring's threading assumption into a checkable
 * fact:
 *
 *   - the shared multi-producer writer (RingBufferWriter::write, behind both
 *     the ingress ring's in_write_lock and the NRT-egress lock) loses no frame
 *     and corrupts none under contention;
 *   - off-audio-thread debug output routes to the LOCKED NRT-out ring, so the
 *     lock-free RT-out ring keeps exactly one writer.
 *
 * Payloads are 8 arbitrary bytes throughout — nothing here parses a frame, so
 * this holds whichever DSP is attached.
 *
 * Catch2 assertion macros are NOT thread-safe, so worker threads only touch
 * plain data / atomics; every REQUIRE/CHECK runs on the main thread after join.
 */
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "shared_memory.h"       // RingCursors layout, EgressRoute, PerformanceMetrics
#include "ring/ring.h"           // Message, MESSAGE_MAGIC
#include "RingBufferWriter.h"    // shared MPSC writer (ingress + NRT-egress)
#include "lanes/ring_drain.h"    // clockwork_drain_ring consumer
#include "LanesFixture.h"        // a core to route through
#include "core_state.h"          // its Egress

// The engine's log, for the debug-egress routing test (audio_processor.cpp).
// Declared directly rather than via audio_processor.h, which pulls in the
// <emscripten/...> shim that is not on this target's include path.
extern "C" int clockwork_log(ClockworkCore* core, const char* fmt, ...);

namespace {

// 8-byte payload identifying (producer, sequence-within-producer) so the drainer
// can prove every frame arrived exactly once with its bytes intact. Arbitrary
// bytes as far as the ring is concerned.
struct Tag { uint32_t producer; uint32_t seq; };

inline void packTag(uint8_t out[8], uint32_t producer, uint32_t seq) {
    std::memcpy(out,     &producer, 4);
    std::memcpy(out + 4, &seq,      4);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Coverage: the shared multi-producer writer under real contention (GREEN).
// N producer threads race RingBufferWriter::write into one ring while a single
// consumer drains it; every distinct frame must arrive exactly once, uncorrupted.
// This is the in_write_lock / NRT-egress-lock path, otherwise untested.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("MPSC ring: concurrent producers and a drainer lose no frames",
          "[RingConcurrency]") {
    constexpr uint32_t kProducers   = 4;
    constexpr uint32_t kPerProducer = 4000;
    constexpr uint32_t kTotal       = kProducers * kPerProducer;

    std::vector<uint8_t> ring(64 * 1024, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<bool>    go{false};
    std::atomic<bool>    producerStuck{false};
    std::atomic<uint32_t> received{0};

    // Owned exclusively by the consumer thread → no synchronisation needed; read
    // on the main thread only after join() establishes happens-before.
    std::vector<uint8_t> seen(kTotal, 0);
    uint32_t duplicates = 0;
    uint32_t badPayload = 0;

    std::thread consumer([&] {
        ClockworkDrainState  st;
        ClockworkDrainMetrics m{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (received.load(std::memory_order_relaxed) < kTotal
               && std::chrono::steady_clock::now() < deadline) {
            clockwork_drain_ring(ring.data(), static_cast<uint32_t>(ring.size()),
                          &head, &tail, st, m, 0,
                [&](uint32_t /*sourceId*/, const uint8_t* payload,
                    uint32_t n, uint32_t /*seq*/) {
                    if (n != sizeof(Tag)) { ++badPayload; return ClockworkDrainVerdict::Consume; }
                    Tag t;
                    std::memcpy(&t, payload, sizeof(t));
                    if (t.producer < kProducers && t.seq < kPerProducer) {
                        const uint32_t idx = t.producer * kPerProducer + t.seq;
                        if (seen[idx]) ++duplicates; else seen[idx] = 1;
                    } else {
                        ++badPayload;
                    }
                    received.fetch_add(1, std::memory_order_relaxed);
                    return ClockworkDrainVerdict::Consume;
                });
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> producers;
    for (uint32_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) { /* start gate: maximise overlap */ }
            for (uint32_t s = 0; s < kPerProducer; ++s) {
                uint8_t payload[sizeof(Tag)];
                packTag(payload, p, s);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (!RingBufferWriter::write(
                           ring.data(), static_cast<uint32_t>(ring.size()),
                           &head, &tail, &sequence, &writeLock,
                           payload, sizeof(payload), p)) {
                    // Ring transiently full; the drainer will free space. Bounded so
                    // a real stall fails loudly instead of hanging the suite.
                    if (std::chrono::steady_clock::now() > deadline) {
                        producerStuck.store(true);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    consumer.join();

    REQUIRE_FALSE(producerStuck.load());
    CHECK(received.load() == kTotal);
    CHECK(duplicates == 0);
    CHECK(badPayload == 0);
    uint32_t missing = 0;
    for (uint8_t v : seen) if (!v) ++missing;
    CHECK(missing == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Off-audio-thread debug routes to NRT-out.
// RT-out (ring_buffer_write) is lock-free — safe ONLY with the audio thread as its
// sole writer. Off the audio thread, the debug emitter must route to the locked
// NRT-out ring instead, so RT-out never gets a second writer. This drives clockwork_log
// from the (non-audio) test thread and asserts the frame lands in NRT-out and
// leaves RT-out untouched; and that with no drainer (self-driven target) it falls
// back to RT-out. Deterministic, no reader thread.
// ─────────────────────────────────────────────────────────────────────────────
TEST_CASE("off-audio-thread clockwork_log routes to NRT-out, not the RT-out ring",
          "[RingConcurrency]") {
    // A core whose egress decides the route (lanes_test::boot), then a
    // self-contained arena + save/restore, so the test does not disturb the
    // engine's for other tests.
    lanes_test::boot();
    clockwork::CoreState& st = *clockwork::state(lanes_test::core());
    std::atomic<bool>& drained = st.egress.nrt_drained;
    static std::vector<uint8_t> scratch(clockwork::kDefaultLayout.arena_bytes, 0);
    uint8_t* const savedBase  = st.arena.at();
    const bool     savedReady = st.arena.ready.load();
    const bool     savedDrn   = drained.load(std::memory_order_relaxed);

    st.arena.base.store(scratch.data());
    RingCursors* c = clockwork::cursors_in(scratch.data(), clockwork::kDefaultLayout);
    c->out_head.store(0);      c->out_tail.store(0);      c->out_sequence.store(0);
    c->nrt_out_head.store(0);  c->nrt_out_tail.store(0);  c->nrt_out_sequence.store(0);
    st.arena.ready.store(true);

    // Drainer present (native NRT gateway): off-thread debug goes to NRT-out and
    // leaves the single-writer RT-out ring untouched.
    drained.store(true, std::memory_order_relaxed);
    clockwork_log(lanes_test::core(), "ring-route-test off-audio-thread line");
    CHECK(c->out_head.load()     == 0);   // RT-out untouched
    CHECK(c->nrt_out_head.load() != 0);   // NRT-out advanced

    // No drainer (self-driven target): falls back to the always-safe RT-out ring.
    drained.store(false, std::memory_order_relaxed);
    clockwork_log(lanes_test::core(), "ring-route-test self-driven-fallback line");
    CHECK(c->out_head.load() != 0);       // RT-out advanced

    st.arena.base.store(savedBase);
    st.arena.ready.store(savedReady);
    drained.store(savedDrn, std::memory_order_relaxed);
}
