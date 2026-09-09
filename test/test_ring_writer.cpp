// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_ring_writer.cpp — the ring writer's decision, at states traffic will
 * not construct for you.
 *
 * The writer refuses in two different ways and they are not the same refusal.
 * One is "there is not that much room anywhere"; the other is "there is room,
 * but not CONTIGUOUSLY before the reader" — the frame would have to restart at
 * offset 0 and would land on bytes the reader has not taken yet.
 *
 * The second is the one that matters and the one nothing else here reaches.
 * It needs the head near the end of the ring, the tail small but not zero, and
 * a frame that fits in the total free space while not fitting in front of the
 * reader. A test that sends messages and pumps blocks never lands on that: the
 * ring is either comfortably empty or comfortably full. Deleting the check
 * entirely leaves an end-to-end suite — and the client-boundary ring cases
 * beside this file — passing, which is how this file came to exist.
 *
 * So the head and tail are set by hand. Nothing is running; the writer is
 * called against a plain buffer, which is the only way to sit exactly on the
 * boundary and stay there.
 */
#include "workers/RingBufferWriter.h"
#include "shared_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <vector>

namespace {

// A ring of one's own: no engine, no segment, no other writer.
struct Ring {
    static constexpr uint32_t SIZE = 4096;
    std::vector<uint8_t>  bytes = std::vector<uint8_t>(SIZE, 0);
    std::atomic<int32_t>  head{0}, tail{0}, seq{0}, lock{0};

    bool write(uint32_t payload, uint32_t at_head, uint32_t at_tail) {
        head.store(static_cast<int32_t>(at_head), std::memory_order_relaxed);
        tail.store(static_cast<int32_t>(at_tail), std::memory_order_relaxed);
        std::vector<uint8_t> data(payload, 0xC5);
        return RingBufferWriter::write(bytes.data(), SIZE, &head, &tail, &seq, &lock,
                                       data.data(), payload, 1);
    }
};

// What a frame of `payload` bytes actually occupies.
uint32_t framed(uint32_t payload) {
    return (static_cast<uint32_t>(sizeof(Message)) + payload + 3u) & ~3u;
}

} // namespace

TEST_CASE("ring writer: an empty ring takes all but the last byte",
          "[ring][writer]") {
    Ring r;
    // head == tail means EMPTY, so a full ring has to stop one byte short —
    // otherwise the two states are the same pair of numbers and a reader
    // cannot tell a brimming ring from a bare one.
    // The frame is aligned up to four, so the largest that fits is the one
    // whose ALIGNED size is still under the limit — not the one whose raw size
    // is. Getting that wrong costs a message at the boundary and nowhere else.
    const uint32_t payload = Ring::SIZE - 4 - sizeof(Message);   // framed: SIZE - 4
    CHECK(r.write(payload, 0, 0) == true);

    Ring r2;
    CHECK(r2.write(payload + 4, 0, 0) == false);   // framed: SIZE, one too many
}

TEST_CASE("ring writer: a frame larger than the ring is refused, however empty",
          "[ring][writer]") {
    Ring r;
    CHECK(r.write(Ring::SIZE, 0, 0) == false);
    CHECK(r.write(Ring::SIZE * 4, 0, 0) == false);
}

TEST_CASE("ring writer: room before the end is taken without wrapping",
          "[ring][writer]") {
    Ring r;
    const uint32_t payload = 64;
    const uint32_t need    = framed(payload);
    // Head positioned so the frame reaches exactly the end. The reader has to
    // be at least one byte along: with tail at 0 the total-space rule bites
    // first, because filling to the end would leave head == tail, which reads
    // as an EMPTY ring rather than a brimming one.
    CHECK(r.write(payload, Ring::SIZE - need, 1) == true);
    Ring r2;
    CHECK(r2.write(payload, Ring::SIZE - need, 0) == false);
}

TEST_CASE("ring writer: wrapping needs room in FRONT of the reader, not merely in total",
          "[ring][writer]") {
    // THE CASE NOTHING ELSE REACHES.
    //
    // The head is near the end, so the frame cannot fit there and must restart
    // at offset 0. Whether that is allowed depends on where the READER is —
    // not on how much space exists altogether. Total space is deliberately
    // ample in every case below, so a writer that only consults the total
    // accepts all of them and overwrites unread bytes.
    const uint32_t payload = 200;
    const uint32_t need    = framed(payload);

    // Head 32 bytes from the end: the frame must wrap.
    const uint32_t head = Ring::SIZE - 32;

    {   // Reader far along: plenty of room at the front. Allowed.
        Ring r;
        CHECK(r.write(payload, head, need + 64) == true);
    }
    {   // Reader one byte short of enough room — the same one-byte rule that
        // keeps full from looking empty. Refused.
        Ring r;
        CHECK(r.write(payload, head, need) == false);
    }
    {   // And one byte past it. Allowed.
        Ring r;
        CHECK(r.write(payload, head, need + 1) == true);
    }
}

TEST_CASE("ring writer: a refused frame writes nothing at all",
          "[ring][writer]") {
    // A writer that gives up part way through leaves a length or a magic
    // behind, and the reader never recovers. Refusal has to be all-or-nothing.
    Ring r;
    std::fill(r.bytes.begin(), r.bytes.end(), 0x7E);

    const uint32_t payload = 200;
    REQUIRE(r.write(payload, Ring::SIZE - 32, 0) == false);

    for (uint8_t b : r.bytes)
        REQUIRE(b == 0x7E);   // untouched, every byte
}
