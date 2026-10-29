// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_lanes.cpp — the guest's regions, from a real engine boot.
 *
 * WHY A WHOLE ENGINE. LanesFixture hands the guest pointers the test itself
 * allocated, so it can say nothing about where an engine gets them from — and
 * that is the whole question here. EngineFixture boots a ClockworkEngine with
 * `shmSegment = false`: no POSIX segment, because there is no second process to share
 * one with. That is the embedded case, and a plugin is the example — its
 * client is the host application, in this process, reading in place.
 *
 * The guest is asked what it was handed, because that is the only place the
 * answer is observable. Sizes AND usability: a NULL region and a region that
 * is a bad pointer are different faults and only the first shows in a size.
 */
#include "EngineFixture.h"
#include "memory_profile.h"
#include "dsp_api.h"
#include "lanes/lanes.h"       // clockwork_arena_header
#include "clockwork_arena.h"
#include "mem_region.h"

#include <utility>

#include <catch2/catch_test_macros.hpp>

using engine_test::Engine;

namespace {

// Ask the guest, and pump until it answers. The engine's control thread wakes
// per block, so a reply needs blocks to ride out on.
osc_test::ParsedReply askGuest(Engine& e, const char* verb, const char* replyAddr) {
    const auto msg = osc_test::message(verb);
    e.engine.ingest(msg.ptr(), msg.size(), 0);
    osc_test::ParsedReply r;
    REQUIRE(e.reply(replyAddr, r));
    return r;
}

} // namespace

TEST_CASE("engine: the guest gets both bulk lanes with no shared segment",
          "[boundary][regions][engine]") {
    Engine e;   // shmSegment = false — the embedded case, no segment

    const auto r = askGuest(e, "/dummy/region/lanes", "/dummy/region/lanes-is");

    // NOT NULL, and the size the profile asks for. A shape like
    // `mShmemCreator ? get_assets() : nullptr` fails here: it hands an
    // embedded guest no returns, and the guest then publishes nothing.
    CHECK(r.argInt(0) == static_cast<int32_t>(CLOCKWORK_ASSETS_BYTES));
    CHECK(r.argInt(1) == static_cast<int32_t>(CLOCKWORK_RETURNS_BYTES));

    // And real memory of that length: the guest read the first, middle and
    // last byte of the assets area, and wrote then read back the same three of the
    // returns. A pointer that is merely non-NULL passes the checks above and
    // fails these.
    CHECK(r.argInt(2) == 1);
    CHECK(r.argInt(3) == 1);
}

TEST_CASE("engine: the arena is there with no shared segment too",
          "[boundary][regions][engine]") {
    Engine e;

    // Clockwork reserves the guest's working memory from its fast tier, sized
    // to the guest's want (the dummy's static figure), on every boot path —
    // not from the segment, which an embedded engine has none of. Asserted
    // here rather than only in the lanes fixture because this is the boot
    // path without a segment.
    const auto r = askGuest(e, "/dummy/region/memory", "/dummy/region/memory-is");

    // The fast span is the fast want (dsp_api.h); the bulk want is a span of
    // its own on every host.
    const DspInfo* info = dsp_describe();
    CHECK(r.argInt(0) == static_cast<int32_t>(info->memory_bytes_wanted));
    CHECK(r.argInt(1) == 1);   // zeroed at dsp_new
    CHECK(r.argInt(2) == 1);   // and writable to its stated length
}

// ── The engine's memory is one block natively too ───────────────────────────
//
// On the web the guest's spans are carved from one block; a desktop engine
// claims one block too (ENGINE_MEMORY), sized to the build, and carves the
// same things from it rather than taking separate mallocs. The arena is not
// in it (static, or a segment other processes map), nor the RT router's
// block (the process's, while the block is a build's).
TEST_CASE("engine: the guest's memory is carved from the engine's one block",
          "[boundary][regions][engine]") {
    Engine e;
    const ClockworkArenaHeader* h = clockwork_arena_header();
    REQUIRE(h != nullptr);
    const auto span = [](const ClockworkArenaAreaEntry* a) {
        const uint64_t base  = (uint64_t)a->base_lo  | ((uint64_t)a->base_hi  << 32);
        const uint64_t bytes = (uint64_t)a->bytes_lo | ((uint64_t)a->bytes_hi << 32);
        return std::pair<uint64_t, uint64_t>{ base, base + bytes };
    };
    const auto* em = clockwork_arena_area(h, CLOCKWORK_AREA_ENGINE_MEMORY);
    REQUIRE(em != nullptr);
    REQUIRE(em->space == CLOCKWORK_SPACE_PROCESS);
    const auto block = span(em);
    CHECK(block.second - block.first >= (uint64_t)CLOCKWORK_GUEST_MEMORY_BULK_BYTES);
    for (uint32_t id : { CLOCKWORK_AREA_GUEST_MEMORY_FAST, CLOCKWORK_AREA_GUEST_MEMORY_BULK }) {
        const auto* a = clockwork_arena_area(h, id);
        REQUIRE(a != nullptr);
        INFO("area " << id);
        CHECK(a->space == CLOCKWORK_SPACE_PROCESS);
        const auto s = span(a);
        CHECK(s.second > s.first);
        CHECK(block.first <= s.first);
        CHECK(s.second <= block.second);
    }
    const auto* rh = clockwork_arena_area(h, CLOCKWORK_AREA_RUST_HEAP);
    REQUIRE(rh != nullptr);
    CHECK(rh->space == CLOCKWORK_SPACE_PROCESS);
    const auto r = span(rh);
    CHECK((r.second <= block.first || r.first >= block.second));
    // The arena is not in the block either.
    const uint64_t arena = (uint64_t)(uintptr_t)h;
    CHECK((arena < block.first || arena >= block.second));
}

// The block is cut by hand and clockwork::mem is never pointed at it:
// natively it stays the system allocator, while an engine runs and after it
// goes. Installing the block as the placement arena would have a fixture that
// boots one engine for the whole process hand every later test's "system"
// memory out of the engine's block, freed as the system's.
TEST_CASE("engine: the engine's block is not the placement arena, while it runs or after",
          "[boundary][regions][engine]") {
    {
        Engine e;
        CHECK(clockwork::mem::arena_size() == 0);
        void* p = clockwork::mem::alloc(clockwork::mem::Tier::Fast, 256);
        REQUIRE(p != nullptr);
        clockwork::mem::free(p);
    }
    CHECK(clockwork::mem::arena_size() == 0);
    CHECK(clockwork::mem::largest_free(clockwork::mem::Tier::Fast) == SIZE_MAX);
    // A system allocation now is the system's, and is freed as such.
    void* p = clockwork::mem::alloc(clockwork::mem::Tier::Fast, 256);
    REQUIRE(p != nullptr);
    clockwork::mem::free(p);
}
