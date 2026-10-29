// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_in_ring_drain.cpp — The audio-thread IN-ring drain must survive a
 * malformed frame without hanging or scanning the whole buffer.
 *
 * The drain in process_audio() consumes OSC frames the transport (or, on
 * native, a cross-process writer to the public shm segment) places in the IN
 * ring. The cursors and frame headers are therefore NOT trusted. These tests
 * inject frames the writer would never produce and assert the drain:
 *   - terminates (a length < header size previously underflowed the payload
 *     size and left the tail un-advanced → an infinite loop on the audio
 *     thread; if that regresses, process_audio() never returns and the test
 *     times out),
 *   - resyncs the tail to head (dropping the suspect region) rather than
 *     walking it one byte at a time, and
 *   - counts the drop.
 *
 * Manual-pump + udpPort 0: this thread is the sole process_audio() caller and
 * the arena is the in-process arena_storage, so writing raw frames into
 * it races nothing.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "lanes/lanes.h"
#include "lanes/ring_drain.h"
#include "shared_memory.h"

#include <atomic>
#include <cstdint>
#include <cstring>

extern "C" uint8_t arena_storage[];

namespace {

ClockworkEngine::Config drainConfig() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;  // test thread owns process_audio()
    return cfg;
}

RingCursors* control() {
    return reinterpret_cast<RingCursors*>(arena_storage + RING_CURSORS_START);
}

// Tick one block on the test thread (channel counts match the fixture's
// 2-in/2-out world) with this thread's frame count and its own time as the
// wall clock (an arbitrary NTP time advanced by the audio rendered), so the
// engine never sees a gap. Returning at all is itself the liveness check.
void pump() {
    static uint64_t frames = 0;
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    time.audio_frames = frames;
    time.wall_ntp     = 4'000'000'000.0 + static_cast<double>(frames) / 48000.0;
    clockwork_tick(&time, 2, 2);
    frames += 128;
}

// Drain whatever boot traffic is queued, then park both cursors at 0 so the
// injected frame sits at a known, non-wrapping offset.
void quiesceAndReset(EngineFixture& fx) {
    for (int i = 0; i < 8; ++i) pump();
    control()->in_head.store(0, std::memory_order_release);
    control()->in_tail.store(0, std::memory_order_release);
    (void)fx;
}

// Write a 16-byte frame header at IN offset 0 and publish head one header past
// it, so the drain sees exactly one frame available.
void injectHeader(uint32_t magic, uint32_t length) {
    uint32_t hdr[4] = { magic, length, /*sequence*/ 0, /*sourceId*/ 0 };
    std::memcpy(arena_storage + IN_RING_START, hdr, sizeof(hdr));
    control()->in_tail.store(0, std::memory_order_release);
    control()->in_head.store(static_cast<int32_t>(sizeof(Message)),
                             std::memory_order_release);
}

} // namespace

TEST_CASE("in-ring-drain: zero-length frame does not hang and is dropped",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore =
        control() ? fx.engine().getMetrics().messages_dropped.load() : 0;

    // length == 0 < sizeof(Message): the pre-fix payload-size computation
    // underflowed and the drop path advanced the tail by length (0), spinning
    // forever. If pump() returns, the lower-bound guard fired.
    injectHeader(MESSAGE_MAGIC, 0);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());  // resynced, not stuck
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: over-long frame is dropped and resynced",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore = fx.engine().getMetrics().messages_dropped.load();

    // length beyond the buffer: must be rejected, not used to index the ring.
    injectHeader(MESSAGE_MAGIC, IN_RING_SIZE + 64);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: a length whose footprint rounding overflows is dropped and resynced",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore = fx.engine().getMetrics().messages_dropped.load();

    // (0xFFFFFFFF + 3) & ~3 is 0 in 32 bits: a footprint of nothing passes
    // every fit check, and the payload the header claims is 4 GB from a
    // pointer inside the ring. Found by fuzz_ring_drain (2026-10-29).
    injectHeader(MESSAGE_MAGIC, 0xFFFFFFFFu);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: a padding marker the writer did not wrap for is corruption and not a loop",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore = fx.engine().getMetrics().messages_dropped.load();

    // A header-only frame at 0, a padding marker at 16, and head at 32: the
    // writer never wrapped (head is past the marker), so following the marker
    // back to 0 would deliver the same frame for ever, every pass. Found by
    // fuzz_ring_drain (2026-10-29).
    const uint32_t frame[4] = { MESSAGE_MAGIC, sizeof(Message), 0, 0 };
    const uint32_t pad[4]   = { PADDING_MAGIC, 0, 0, 0 };
    std::memcpy(arena_storage + IN_RING_START, frame, sizeof frame);
    std::memcpy(arena_storage + IN_RING_START + sizeof frame, pad, sizeof pad);
    control()->in_tail.store(0, std::memory_order_release);
    control()->in_head.store(static_cast<int32_t>(2 * sizeof(Message)), std::memory_order_release);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

// The drain's own sequence-gap arithmetic, on a local ring: the sequence
// after 0x7FFFFFFF is 0 and counts no gap; a real gap counts its size.
TEST_CASE("ring-drain: the sequence wraps at 31 bits without a gap and without overflow",
          "[ring][security]") {
    alignas(4) uint8_t ring[256] = {};
    const uint32_t frames[3][4] = {
        { MESSAGE_MAGIC, sizeof(Message), 0x7FFFFFFFu, 0 },
        { MESSAGE_MAGIC, sizeof(Message), 0u, 0 },
        { MESSAGE_MAGIC, sizeof(Message), 5u, 0 },   // 1..4 missing: a gap of 4
    };
    std::memcpy(ring, frames, sizeof frames);
    std::atomic<int32_t> head{3 * static_cast<int32_t>(sizeof(Message))}, tail{0};
    std::atomic<uint32_t> gaps{0}, corrupted{0};
    ClockworkDrainState st;
    ClockworkDrainMetrics metrics{ nullptr, nullptr, &corrupted, &gaps };
    const uint32_t consumed = clockwork_drain_ring(ring, sizeof ring, &head, &tail, st, metrics, 0,
        [](uint32_t, const uint8_t*, uint32_t, uint32_t) { return ClockworkDrainVerdict::Consume; });
    CHECK(consumed == 3);
    CHECK(corrupted.load() == 0);
    CHECK(gaps.load() == 4);
    CHECK(tail.load() == head.load());
}

TEST_CASE("in-ring-drain: bad magic resyncs to head instead of byte-walking",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    const uint32_t droppedBefore = fx.engine().getMetrics().messages_dropped.load();

    // A garbage header followed by valid bytes up to head: the drain must jump
    // the tail straight to head (the whole region after corruption is suspect),
    // not advance one byte per iteration scanning for the next magic.
    injectHeader(0xABADCAFE, 64);
    control()->in_head.store(static_cast<int32_t>(IN_RING_SIZE / 2),
                             std::memory_order_release);
    pump();

    auto* ctrl = control();
    CHECK(ctrl->in_tail.load() == ctrl->in_head.load());
    CHECK(fx.engine().getMetrics().messages_dropped.load() == droppedBefore + 1);
}

TEST_CASE("in-ring-drain: out-of-range cursor is clamped, drain skipped safely",
          "[ingress][ring][security]") {
    EngineFixture fx(drainConfig());
    quiesceAndReset(fx);

    // A tail outside [0, IN_RING_SIZE) would index outside the IN region.
    // The drain must repair it (resync tail to head) and not dereference it.
    control()->in_head.store(0, std::memory_order_release);
    control()->in_tail.store(0x7FFFFFFF, std::memory_order_release);
    pump();  // must return without an OOB read

    CHECK(static_cast<uint32_t>(control()->in_tail.load()) < IN_RING_SIZE);
}
