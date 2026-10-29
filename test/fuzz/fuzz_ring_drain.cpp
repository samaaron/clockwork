// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The ring drain (src/lanes/ring_drain.h) over a ring and cursors another
// process wrote: the shared-memory peer plane's command ring, which a peer
// fills directly. The drain promises to read nothing outside the ring,
// write nothing but the tail, deliver only frames that lie whole inside the
// ring, stop within maxFrames, and leave the tail in range. The input is the
// head and tail (4 bytes each, as the cursors are), maxFrames, a retain
// pattern, then the ring's bytes.

#include "lanes/ring_drain.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kRing  = 1024;
constexpr uint32_t kGuard = 64;
constexpr uint8_t  kSentinel = 0xA5;

[[noreturn]] void fail() { std::abort(); }

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 10) return 0;
    int32_t head, tail;
    std::memcpy(&head, data, 4);
    std::memcpy(&tail, data + 4, 4);
    const uint32_t maxFrames = data[8];
    const uint8_t  retainEvery = data[9];
    data += 10; size -= 10;

    // The ring between two sentinel blocks: a read or write past either end
    // is the sanitizer's to catch; a write inside the ring is checked below.
    static uint8_t block[kGuard + kRing + kGuard];
    std::memset(block, kSentinel, sizeof block);
    uint8_t* ring = block + kGuard;
    std::memset(ring, 0, kRing);
    std::memcpy(ring, data, size < kRing ? size : kRing);
    static uint8_t before[kRing];
    std::memcpy(before, ring, kRing);

    std::atomic<int32_t> headA{head}, tailA{tail};
    std::atomic<uint32_t> received{0}, bytes{0}, corrupted{0}, gaps{0};
    ClockworkDrainState st;
    ClockworkDrainMetrics metrics{ &received, &bytes, &corrupted, &gaps };

    for (int pass = 0; pass < 2; ++pass) {   // the second pass sees a repaired tail
        uint32_t delivered = 0;
        ClockworkDrainStop stop;
        const uint32_t consumed = clockwork_drain_ring(
            ring, kRing, &headA, &tailA, st, metrics, maxFrames,
            [&](uint32_t, const uint8_t* payload, uint32_t n, uint32_t) {
                if (n == 0 || payload < ring || payload + n > ring + kRing) fail();
                volatile uint8_t sink = payload[0];
                sink = payload[n - 1];
                (void)sink;
                ++delivered;
                return (retainEvery && delivered % retainEvery == 0) ? ClockworkDrainVerdict::Retain
                                                                      : ClockworkDrainVerdict::Consume;
            },
            &stop);
        if (maxFrames && consumed > maxFrames) fail();
        if (delivered > consumed + 1) fail();   // at most the retained one over
        const uint32_t t = static_cast<uint32_t>(tailA.load());
        if (t >= kRing && static_cast<uint32_t>(headA.load()) < kRing) fail();   // a bad tail is repaired to head
        if (headA.load() != head) fail();       // the head is the producer's
        if (std::memcmp(ring, before, kRing) != 0) fail();   // the drain reads
        for (uint32_t i = 0; i < kGuard; ++i)
            if (block[i] != kSentinel || block[kGuard + kRing + i] != kSentinel) fail();
    }
    return 0;
}
