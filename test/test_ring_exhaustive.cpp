// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The message ring, exhaustively, for small rings: every cursor position a
// frame can start from and every payload length a ring can hold, one frame
// and then two, written through RingBufferWriter and read back through
// clockwork_drain_ring. What is proved for these sizes, by enumeration rather
// than by a model checker (the writer and the drain are C++ over std::atomic,
// which CBMC's front end does not model):
//
//   - the writer never touches a byte outside the ring: the ring sits inside
//     a guarded block whose sentinels are checked after every write;
//   - a frame that fits is read back whole, bytes and sequence, and one that
//     does not fit is refused with the ring left as it was;
//   - two frames come back in order, across the ring's end as much as not;
//   - the ring is empty after the drain, head and tail agreed.
//
// The frame arithmetic has no size-dependent branches beyond the wrap, so the
// three ring sizes here cover the cases the production rings (much larger,
// powers of two) can reach: a frame against the end, a frame that wraps, a
// pad marker with and without room for it, a ring one byte from full.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lanes/ring_drain.h"
#include "ring/ring.h"
#include "workers/RingBufferWriter.h"

namespace {

constexpr uint32_t kGuard = 64;        // sentinel bytes either side of the ring
constexpr uint8_t  kSentinel = 0xA5;

struct Ring {
    std::vector<uint8_t> block;   // guard | ring | guard
    uint32_t size;
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, lock{0};
    explicit Ring(uint32_t n, uint32_t cursor) : block(kGuard + n + kGuard, kSentinel), size(n) {
        std::memset(ring(), 0, n);
        head.store(static_cast<int32_t>(cursor));
        tail.store(static_cast<int32_t>(cursor));
    }
    uint8_t* ring() { return block.data() + kGuard; }
    bool guardsIntact() const {
        for (uint32_t i = 0; i < kGuard; ++i)
            if (block[i] != kSentinel || block[kGuard + size + i] != kSentinel) return false;
        return true;
    }
    bool write(const std::vector<uint8_t>& payload, uint32_t source) {
        return RingBufferWriter::write(ring(), size, &head, &tail, &sequence, &lock,
                                       payload.data(), static_cast<uint32_t>(payload.size()), source);
    }
    struct Frame { uint32_t source; std::vector<uint8_t> payload; uint32_t sequence; };
    std::vector<Frame> drain() {
        std::vector<Frame> out;
        ClockworkDrainState st;
        ClockworkDrainMetrics metrics;
        clockwork_drain_ring(ring(), size, &head, &tail, st, metrics, 0,
            [&](uint32_t source, const uint8_t* p, uint32_t n, uint32_t seq) {
                out.push_back(Frame{source, std::vector<uint8_t>(p, p + n), seq});
                return ClockworkDrainVerdict::Consume;
            });
        return out;
    }
    bool empty() const { return head.load() == tail.load(); }
};

std::vector<uint8_t> pattern(uint32_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i * 7u);
    return v;
}

// The largest payload a ring of this size holds in one frame: the header, the
// payload rounded up to 4, and one byte the ring keeps between head and tail.
uint32_t largestPayload(uint32_t size) {
    for (uint32_t p = size; p > 0; --p) {
        const uint32_t frame = (static_cast<uint32_t>(sizeof(Message)) + p + 3u) & ~3u;
        if (frame <= size - 1) return p;
    }
    return 0;
}

}  // namespace

TEST_CASE("ring: one frame from every cursor at every payload length, in bounds and read back whole",
          "[ring][exhaustive]") {
    for (uint32_t size : {64u, 96u, 128u}) {
        const uint32_t maxPayload = largestPayload(size);
        for (uint32_t cursor = 0; cursor < size; cursor += 4) {
            for (uint32_t n = 0; n <= size; ++n) {
                Ring r(size, cursor);
                const auto payload = pattern(n, static_cast<uint8_t>(cursor + n));
                const bool wrote = r.write(payload, 7u);
                INFO("size " << size << " cursor " << cursor << " payload " << n);
                REQUIRE(r.guardsIntact());
                if (!wrote) {
                    // refused: too big for the ring, or for the room this cursor leaves
                    const bool tooBig = n > maxPayload;
                    const uint32_t frame = (static_cast<uint32_t>(sizeof(Message)) + n + 3u) & ~3u;
                    const bool noRoomAtEnd = frame > size - cursor && frame > (cursor > 0 ? cursor - 1 : 0);
                    REQUIRE((tooBig || noRoomAtEnd));
                    REQUIRE(r.empty());
                    REQUIRE(r.drain().empty());
                    continue;
                }
                const auto frames = r.drain();
                // a frame with no payload is consumed, never delivered (ring_drain.h)
                REQUIRE(frames.size() == (n == 0 ? 0u : 1u));
                if (n > 0) {
                    CHECK(frames[0].source == 7u);
                    CHECK(frames[0].payload == payload);
                    CHECK(frames[0].sequence == 0u);
                }
                CHECK(r.empty());
                REQUIRE(r.guardsIntact());
            }
        }
    }
}

TEST_CASE("ring: two frames from every cursor come back in order, wrapping or not",
          "[ring][exhaustive]") {
    const uint32_t size = 64;
    for (uint32_t cursor = 0; cursor < size; cursor += 4) {
        for (uint32_t a = 0; a <= 24; ++a) {
            for (uint32_t b = 0; b <= 24; ++b) {
                Ring r(size, cursor);
                const auto pa = pattern(a, 1), pb = pattern(b, 101);
                const bool wa = r.write(pa, 1u);
                REQUIRE(r.guardsIntact());
                const bool wb = wa && r.write(pb, 2u);
                REQUIRE(r.guardsIntact());
                INFO("cursor " << cursor << " a " << a << " b " << b << " wrote " << wa << wb);
                const auto frames = r.drain();
                // delivered: the frames that carry a payload, in order; an empty one is consumed unseen
                std::vector<Ring::Frame> expected;
                if (wa && a > 0) expected.push_back(Ring::Frame{1u, pa, 0u});
                if (wb && b > 0) expected.push_back(Ring::Frame{2u, pb, 1u});
                REQUIRE(frames.size() == expected.size());
                for (size_t i = 0; i < expected.size(); ++i) {
                    CHECK(frames[i].source == expected[i].source);
                    CHECK(frames[i].payload == expected[i].payload);
                    CHECK(frames[i].sequence == expected[i].sequence);
                }
                CHECK(r.empty());
                REQUIRE(r.guardsIntact());
            }
        }
    }
}
