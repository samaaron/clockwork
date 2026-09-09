// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_mem_arena.cpp — clockwork::mem's host-supplied arena.
 *
 * The arena exists so one allocation path serves every target. wasm cannot
 * take an 8 MB real-time pool from emscripten's malloc — that heap grows
 * upward into the guest's region — so the host hands clockwork a span and
 * every allocation comes from inside it. Native hands over nothing and keeps
 * std::malloc. Callers are identical either way, and these cases pin that:
 * the same alloc()/free() calls must behave the same with and without a span.
 *
 * The arena's traffic in production is a handful of large, long-lived blocks
 * taken at boot, but it is a real allocator and is tested as one — splitting,
 * both-way coalescing, exhaustion, and the boundaries where those meet.
 */
#include <catch2/catch_test_macros.hpp>

#include "mem_region.h"

#include <cstdint>
#include <cstring>
#include <vector>

using clockwork::mem::Tier;

namespace {

// A span with room for several blocks plus their headers. Heap-allocated
// rather than a static, so ASan can see a write that runs off either end.
struct ArenaFixture {
    static constexpr size_t kBytes = 64 * 1024;
    std::vector<uint8_t> backing;

    ArenaFixture() : backing(kBytes + 64, 0xAB) {
        clockwork::mem::set_arena(backing.data(), kBytes);
    }
    ~ArenaFixture() { clockwork::mem::set_arena(nullptr, 0); }

    uint8_t* base() { return backing.data(); }
    bool contains(void* p) const {
        auto* c = static_cast<const uint8_t*>(p);
        return c >= backing.data() && c < backing.data() + kBytes;
    }
};

bool aligned16(void* p) { return (reinterpret_cast<uintptr_t>(p) & 15u) == 0; }

} // namespace

// ── No arena: unchanged behaviour ───────────────────────────────────────────

TEST_CASE("mem arena: without a span, allocation is the system's", "[mem]") {
    // largest_free reporting SIZE_MAX is what tells the heap and the RT pool
    // "no region bounds you, your compile-time cap wins" — the behaviour every
    // native build has always had.
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) == SIZE_MAX);
    REQUIRE(clockwork::mem::arena_size() == 0);
    REQUIRE(clockwork::mem::arena_available() == 0);

    void* p = clockwork::mem::alloc(Tier::Fast, 1024);
    REQUIRE(p != nullptr);
    REQUIRE(aligned16(p));
    std::memset(p, 0x5A, 1024);   // writable for its whole length
    clockwork::mem::free(p);
}

TEST_CASE("mem arena: a zero-byte request is null on either backend", "[mem]") {
    REQUIRE(clockwork::mem::alloc(Tier::Fast, 0) == nullptr);
    ArenaFixture fix;
    REQUIRE(clockwork::mem::alloc(Tier::Fast, 0) == nullptr);
}

// ── With a span: every allocation comes from inside it ──────────────────────

TEST_CASE("mem arena: allocations land inside the span", "[mem]") {
    ArenaFixture fix;

    REQUIRE(clockwork::mem::arena_size() >= ArenaFixture::kBytes - 16);
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) < ArenaFixture::kBytes);
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) > 0);

    void* a = clockwork::mem::alloc(Tier::Fast, 1000);
    void* b = clockwork::mem::alloc(Tier::Bulk, 2000);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(fix.contains(a));
    REQUIRE(fix.contains(b));
    REQUIRE(a != b);
    REQUIRE(aligned16(a));
    REQUIRE(aligned16(b));

    // Both tiers share the one span — a single-region platform has one kind of
    // memory, so the tier stays documentation of intent.
    clockwork::mem::free(a);
    clockwork::mem::free(b);
}

TEST_CASE("mem arena: distinct live allocations never overlap", "[mem]") {
    ArenaFixture fix;

    struct Span { uint8_t* p; size_t n; };
    std::vector<Span> live;
    // Ragged sizes so splits leave remainders that are not multiples of each
    // other — an overlap bug needs unequal neighbours to show up.
    for (size_t n : {17u, 512u, 33u, 1024u, 7u, 2048u, 129u}) {
        auto* p = static_cast<uint8_t*>(clockwork::mem::alloc(Tier::Fast, n));
        REQUIRE(p != nullptr);
        REQUIRE(aligned16(p));
        std::memset(p, static_cast<int>(live.size() + 1), n);
        live.push_back({p, n});
    }

    // Every byte still reads back as its own allocation's fill.
    for (size_t i = 0; i < live.size(); ++i)
        for (size_t j = 0; j < live[i].n; ++j)
            REQUIRE(live[i].p[j] == static_cast<uint8_t>(i + 1));

    for (auto& s : live)
        clockwork::mem::free(s.p);
}

TEST_CASE("mem arena: in_use tracks what is handed out", "[mem]") {
    ArenaFixture fix;
    REQUIRE(clockwork::mem::in_use(Tier::Fast) == 0);

    void* a = clockwork::mem::alloc(Tier::Fast, 4096);
    REQUIRE(clockwork::mem::in_use(Tier::Fast) >= 4096);
    const size_t withOne = clockwork::mem::in_use(Tier::Fast);

    void* b = clockwork::mem::alloc(Tier::Fast, 4096);
    REQUIRE(clockwork::mem::in_use(Tier::Fast) > withOne);

    clockwork::mem::free(b);
    clockwork::mem::free(a);
    REQUIRE(clockwork::mem::in_use(Tier::Fast) == 0);
}

// ── Reuse and coalescing ────────────────────────────────────────────────────

TEST_CASE("mem arena: freed space is reused", "[mem]") {
    ArenaFixture fix;

    void* a = clockwork::mem::alloc(Tier::Fast, 8192);
    REQUIRE(a != nullptr);
    clockwork::mem::free(a);
    void* b = clockwork::mem::alloc(Tier::Fast, 8192);
    REQUIRE(b == a);   // first fit walks back onto the block just released
    clockwork::mem::free(b);
}

TEST_CASE("mem arena: neighbours coalesce in both directions", "[mem]") {
    ArenaFixture fix;
    const size_t whole = clockwork::mem::largest_free(Tier::Fast);

    // Three adjacent blocks; free the middle one last so the survivor has to
    // absorb a free neighbour on each side in one call.
    void* a = clockwork::mem::alloc(Tier::Fast, 4096);
    void* b = clockwork::mem::alloc(Tier::Fast, 4096);
    void* c = clockwork::mem::alloc(Tier::Fast, 4096);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);

    // Take the whole tail as well, or a 12000-byte request is answered out of
    // untouched space and proves nothing about merging.
    void* tail = clockwork::mem::alloc(Tier::Fast, clockwork::mem::largest_free(Tier::Fast));
    REQUIRE(tail != nullptr);

    clockwork::mem::free(a);
    clockwork::mem::free(c);
    // a and c are now the only free space and they are not adjacent: 4096 each,
    // and nothing may merge across the live block between them.
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) == 4096);
    REQUIRE(clockwork::mem::alloc(Tier::Fast, 12000) == nullptr);

    // Freeing the middle joins all three in one call — forward onto c, then
    // backward from a — so the span is larger than the sum of the payloads by
    // the two headers that stop being headers.
    clockwork::mem::free(b);
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) > 3 * 4096);

    void* big = clockwork::mem::alloc(Tier::Fast, 12000);
    REQUIRE(big != nullptr);
    REQUIRE(big == a);           // the merged block starts where a did
    REQUIRE(fix.contains(big));

    clockwork::mem::free(big);
    clockwork::mem::free(tail);
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) == whole);
}

TEST_CASE("mem arena: repeated churn does not leak the span away", "[mem]") {
    ArenaFixture fix;
    const size_t whole = clockwork::mem::largest_free(Tier::Fast);

    // If coalescing missed a case, the largest free block shrinks a little on
    // every pass and this fails long before the loop ends.
    for (int pass = 0; pass < 500; ++pass) {
        void* x = clockwork::mem::alloc(Tier::Fast, 1024 + (pass % 7) * 64);
        void* y = clockwork::mem::alloc(Tier::Fast, 2048);
        REQUIRE(x != nullptr);
        REQUIRE(y != nullptr);
        clockwork::mem::free(x);
        clockwork::mem::free(y);
    }
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) == whole);
    REQUIRE(clockwork::mem::in_use(Tier::Fast) == 0);
}

// ── Exhaustion: bounded means it fails, not that it escapes ─────────────────

TEST_CASE("mem arena: a request larger than the span fails", "[mem]") {
    ArenaFixture fix;

    // The point of the arena. Under malloc this would succeed and, on wasm,
    // hand back memory inside the guest's region.
    REQUIRE(clockwork::mem::alloc(Tier::Fast, ArenaFixture::kBytes * 4) == nullptr);
    REQUIRE(clockwork::mem::alloc(Tier::Bulk, ArenaFixture::kBytes * 4) == nullptr);

    // And the arena is still intact and usable afterwards.
    void* p = clockwork::mem::alloc(Tier::Fast, 1024);
    REQUIRE(p != nullptr);
    REQUIRE(fix.contains(p));
    clockwork::mem::free(p);
}

TEST_CASE("mem arena: allocating until empty then releasing restores it", "[mem]") {
    ArenaFixture fix;
    const size_t whole = clockwork::mem::largest_free(Tier::Fast);

    std::vector<void*> all;
    while (void* p = clockwork::mem::alloc(Tier::Fast, 1024)) {
        REQUIRE(fix.contains(p));
        all.push_back(p);
        REQUIRE(all.size() < 1000);   // must terminate: the span is bounded
    }
    REQUIRE(!all.empty());
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) < 1024);

    for (void* p : all)
        clockwork::mem::free(p);
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) == whole);
}

TEST_CASE("mem arena: largest_free is honest about what will fit", "[mem]") {
    ArenaFixture fix;

    void* a = clockwork::mem::alloc(Tier::Fast, 20000);
    REQUIRE(a != nullptr);

    // Whatever it reports must actually be allocatable, and one byte past the
    // last 16-byte slot must not be. This is the number the heap and the RT
    // pool size themselves against, so an optimistic answer is a boot failure.
    const size_t avail = clockwork::mem::largest_free(Tier::Fast);
    void* fits = clockwork::mem::alloc(Tier::Fast, avail);
    REQUIRE(fits != nullptr);
    clockwork::mem::free(fits);

    REQUIRE(clockwork::mem::alloc(Tier::Fast, avail + 16) == nullptr);
    clockwork::mem::free(a);
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

TEST_CASE("mem arena: clearing the span restores the system allocator", "[mem]") {
    {
        ArenaFixture fix;
        void* inside = clockwork::mem::alloc(Tier::Fast, 128);
        REQUIRE(fix.contains(inside));
        clockwork::mem::free(inside);
    }
    REQUIRE(clockwork::mem::arena_size() == 0);
    REQUIRE(clockwork::mem::largest_free(Tier::Fast) == SIZE_MAX);

    void* p = clockwork::mem::alloc(Tier::Fast, 128);
    REQUIRE(p != nullptr);
    clockwork::mem::free(p);
}

TEST_CASE("mem arena: a span too small to hold one block is refused", "[mem]") {
    uint8_t tiny[8] = {};
    clockwork::mem::set_arena(tiny, sizeof(tiny));
    // Refused rather than half-configured: allocation falls back to the system
    // instead of writing a header past the end of an 8-byte span.
    REQUIRE(clockwork::mem::arena_size() == 0);
    void* p = clockwork::mem::alloc(Tier::Fast, 64);
    REQUIRE(p != nullptr);
    clockwork::mem::free(p);
    clockwork::mem::set_arena(nullptr, 0);
}

TEST_CASE("mem arena: a misaligned base still yields aligned payloads", "[mem]") {
    std::vector<uint8_t> backing(16 * 1024, 0);
    // Deliberately odd: the arena must align up and pay for it out of its own
    // span rather than handing back a payload the engine cannot use.
    clockwork::mem::set_arena(backing.data() + 1, backing.size() - 1);

    void* a = clockwork::mem::alloc(Tier::Fast, 100);
    void* b = clockwork::mem::alloc(Tier::Fast, 100);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(aligned16(a));
    REQUIRE(aligned16(b));

    auto* end = backing.data() + backing.size();
    REQUIRE(static_cast<uint8_t*>(b) + 100 <= end);

    clockwork::mem::free(a);
    clockwork::mem::free(b);
    clockwork::mem::set_arena(nullptr, 0);
}

TEST_CASE("mem arena: freeing a pointer from outside the span is safe", "[mem]") {
    void* outside = clockwork::mem::alloc(Tier::Fast, 256);   // taken from the system
    REQUIRE(outside != nullptr);

    ArenaFixture fix;
    // Routed back to the system by address range rather than treated as a
    // block header — the case that arises whenever a span is installed while
    // earlier allocations are still live.
    clockwork::mem::free(outside);

    void* inside = clockwork::mem::alloc(Tier::Fast, 256);
    REQUIRE(fix.contains(inside));
    clockwork::mem::free(inside);
    clockwork::mem::free(nullptr);   // and null is a no-op on both paths
}
