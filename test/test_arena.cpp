// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_arena.cpp — the arena's own table (clockwork_arena.h).
 *
 * The constants in shared_memory.h lay the arena out; writeArenaHeader turns
 * them into the table every reader takes the layout from. These pin the
 * contract: the table describes the constants exactly, the five areas lie
 * end to end in their halves, every region lies in the area the model puts
 * it in with its five facts stated, and a reader refuses what it cannot use.
 */
#include <catch2/catch_test_macros.hpp>
#include "shared_memory.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <iterator>
#include <vector>

namespace {
std::vector<uint8_t> arena() {
    std::vector<uint8_t> a(ARENA_SIZE, 0);
    writeArenaHeader(a.data());
    return a;
}
const ClockworkArenaEntry* entry(const std::vector<uint8_t>& a, uint32_t id) {
    return clockwork_arena_find(arenaHeader(a.data()), id);
}
ClockworkArenaEntry* mutable_entry(std::vector<uint8_t>& a, uint32_t id) {
    return const_cast<ClockworkArenaEntry*>(entry(a, id));
}
}

TEST_CASE("arena: the header is the front of the arena and says so", "[arena]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    CHECK(h->magic == CLOCKWORK_ARENA_MAGIC);
    CHECK(h->version == CLOCKWORK_ARENA_VERSION);
    CHECK(h->state == CLOCKWORK_ARENA_PUBLISHED);
    CHECK(h->header_bytes == ARENA_HEADER_SIZE);
    CHECK(h->arena_bytes == ARENA_SIZE);
    CHECK(h->area_bytes == sizeof(ClockworkArenaAreaEntry));
    CHECK(h->entry_bytes == sizeof(ClockworkArenaEntry));
    CHECK(sizeof(ClockworkArenaAreaEntry) == 24);
    CHECK(sizeof(ClockworkArenaEntry) == 64);
    CHECK(sizeof(ClockworkArenaHeader) <= ARENA_HEADER_SIZE);
    // The two halves, edge to edge, covering the whole arena.
    CHECK(h->clockwork_bytes == CLOCKWORK_HALF_SIZE);
    CHECK(h->guest_offset == h->clockwork_bytes);
    CHECK(h->guest_offset + h->guest_bytes == h->arena_bytes);
    const char* why = "";
    CHECK(clockwork_arena_check(h, ARENA_SIZE, &why));

    // A new arena holds no instance yet, and no generation has opened: the
    // word is odd, and the generation it names is 0 (docs/RESTARTS.md).
    CHECK(h->generation == 1);
    CHECK_FALSE(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == 0);
}

TEST_CASE("arena: the generation word says whether there is an instance, and which one", "[arena]") {
    // One word, read by every side with one load: the low bit is "between
    // instances", the rest is the generation the last build opened — the
    // number /clockwork/setup named. It moves on only at a build.
    auto a = arena();
    auto* h = const_cast<ClockworkArenaHeader*>(arenaHeader(a.data()));
    h->generation = 2;                           // the first instance is up
    CHECK(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == 1);
    h->generation = 3;                           // torn down; nothing built yet
    CHECK_FALSE(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == 1);   // the one closing is still the one named
    h->generation = 4;                           // the second is up
    CHECK(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == 2);
    // The header is the one region that outlives a rebuild, and the word
    // must not move an offset: it takes the first reserved slot.
    CHECK(offsetof(ClockworkArenaHeader, generation) == offsetof(ClockworkArenaHeader, state) + 4);
    CHECK(offsetof(ClockworkArenaHeader, areas) == offsetof(ClockworkArenaHeader, generation) + 4 + sizeof(h->reserved));
}

namespace {
// Every area in the arena: where the constants put it, in which half.
struct ExpectArea { uint32_t area, start, end; bool guests; };
const ExpectArea kAreas[] = {
    { CLOCKWORK_AREA_CLOCKWORK_STATE,     CLOCKWORK_STATE_START,     CLOCKWORK_STATE_END,     false },
    { CLOCKWORK_AREA_CLOCKWORK_AUDIO,     CLOCKWORK_AUDIO_START,     CLOCKWORK_AUDIO_END,     false },
    { CLOCKWORK_AREA_CLOCKWORK_TRANSPORT, CLOCKWORK_TRANSPORT_START, CLOCKWORK_TRANSPORT_END, false },
    { CLOCKWORK_AREA_GUEST_PUBLIC,        GUEST_PUBLIC_START,        GUEST_PUBLIC_END,        true },
    { CLOCKWORK_AREA_GUEST_PRIVATE,       GUEST_PRIVATE_START,       GUEST_PRIVATE_END,       true },
};
// Every region: where the constants put it, and its five facts.
struct Expect { uint32_t id, start, size, owner, audience, cadence, tier, lifetime, area; };
constexpr uint32_t PUB = CLOCKWORK_AUDIENCE_PUBLIC, GUEST = CLOCKWORK_AUDIENCE_GUEST,
                   HOST = CLOCKWORK_AUDIENCE_HOST, TRANSPORT = CLOCKWORK_AUDIENCE_TRANSPORT;
constexpr uint32_t BLOCK = CLOCKWORK_CADENCE_BLOCK, CHANGE = CLOCKWORK_CADENCE_CHANGE, BOOT = CLOCKWORK_CADENCE_BOOT;
constexpr uint32_t FAST = CLOCKWORK_TIER_FAST, BULK = CLOCKWORK_TIER_BULK;
constexpr uint32_t COLD = CLOCKWORK_LIFETIME_COLD_BOOT, REBUILD = CLOCKWORK_LIFETIME_REBUILD, ARENA = CLOCKWORK_LIFETIME_ARENA;
const Expect kExpect[] = {
    // clockwork-state
    { CLOCKWORK_ARENA_CLOCKWORK_METRICS,         METRICS_START,         METRICS_SIZE,         CLOCKWORK_OWNER_CLOCKWORK, PUB,         BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_STATE },
    { CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS,    NATIVE_STATS_START,    NATIVE_STATS_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, PUB,         BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_STATE },
    { CLOCKWORK_ARENA_CLOCKWORK_CLOCK_ANCHORS,   CLOCK_ANCHORS_START,   CLOCK_ANCHORS_SIZE,   CLOCKWORK_OWNER_HOST,      PUB,         BLOCK,  FAST, COLD,    CLOCKWORK_AREA_CLOCKWORK_STATE },
    { CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE,     CLOCK_STATE_START,     CLOCK_STATE_SIZE,     CLOCKWORK_OWNER_CLOCKWORK, PUB | GUEST, BLOCK,  FAST, COLD,    CLOCKWORK_AREA_CLOCKWORK_STATE },
    { CLOCKWORK_ARENA_CLOCKWORK_SAMPLE_CLOCK,    SAMPLE_CLOCK_START,    SAMPLE_CLOCK_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, PUB,         BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_STATE },
    // Cold boot, not rebuild: the streams bound over channels outlive the
    // guest instance (docs/RESTARTS.md, "port routing"); a device change
    // rewrites the device's own words, as any attach rewrites an entry.
    { CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP,     CHANNEL_MAP_START,     CHANNEL_MAP_SIZE,     CLOCKWORK_OWNER_CLOCKWORK, PUB | GUEST, CHANGE, FAST, COLD,    CLOCKWORK_AREA_CLOCKWORK_STATE },
    // clockwork-audio
    { CLOCKWORK_ARENA_CLOCKWORK_SCOPE,           SCOPE_START,           SCOPE_SIZE,           CLOCKWORK_OWNER_CLOCKWORK, PUB,         BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_AUDIO },
    { CLOCKWORK_ARENA_CLOCKWORK_TAPS,            TAPS_START,            TAPS_SIZE,            CLOCKWORK_OWNER_CLOCKWORK, PUB,         BLOCK,  BULK, REBUILD, CLOCKWORK_AREA_CLOCKWORK_AUDIO },
    // clockwork-transport
    { CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS,    RING_CURSORS_START,    RING_CURSORS_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT,   BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_TRANSPORT },
    { CLOCKWORK_ARENA_CLOCKWORK_NODE_ID_COUNTER, NODE_ID_COUNTER_START, NODE_ID_COUNTER_SIZE, CLOCKWORK_OWNER_CLIENT,    TRANSPORT,   CHANGE, FAST, COLD,    CLOCKWORK_AREA_CLOCKWORK_TRANSPORT },
    { CLOCKWORK_ARENA_CLOCKWORK_IN_RING,         IN_RING_START,         IN_RING_SIZE,         CLOCKWORK_OWNER_CLIENT,    TRANSPORT,   BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_TRANSPORT },
    { CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING,     RT_OUT_RING_START,     RT_OUT_RING_SIZE,     CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT,   BLOCK,  FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_TRANSPORT },
    { CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING,    NRT_OUT_RING_START,    NRT_OUT_RING_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT,   CHANGE, FAST, REBUILD, CLOCKWORK_AREA_CLOCKWORK_TRANSPORT },
    // Clockwork's own control verbs, written at the boundary and drained by
    // the control pass with no audio block in the loop: lifetime arena, so a
    // rebuild neither resets it nor has anything of clockwork's to carry.
    { CLOCKWORK_ARENA_CLOCKWORK_CONTROL_RING,    CONTROL_RING_START,    CONTROL_RING_SIZE,    CLOCKWORK_OWNER_CLIENT,    TRANSPORT,   CHANGE, FAST, ARENA,   CLOCKWORK_AREA_CLOCKWORK_TRANSPORT },
    // guest-public
    { CLOCKWORK_ARENA_GUEST_WINDOW,              GUEST_WINDOW_START,    GUEST_WINDOW_SIZE,    CLOCKWORK_OWNER_GUEST,     PUB,         CHANGE, BULK, REBUILD, CLOCKWORK_AREA_GUEST_PUBLIC },
    // guest-private
    { CLOCKWORK_ARENA_GUEST_CONFIG,              GUEST_CONFIG_START,    GUEST_CONFIG_SIZE,    CLOCKWORK_OWNER_HOST,      GUEST | HOST, BOOT,  BULK, COLD,    CLOCKWORK_AREA_GUEST_PRIVATE },
    { CLOCKWORK_ARENA_GUEST_PERSIST,             GUEST_PERSIST_START,   GUEST_PERSIST_SIZE,   CLOCKWORK_OWNER_GUEST,     GUEST,       CHANGE, BULK, COLD,    CLOCKWORK_AREA_GUEST_PRIVATE },
};
} // namespace

TEST_CASE("arena: the five areas lie end to end, each in its half", "[arena]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    CHECK(h->area_count == 14);
    uint32_t next = h->header_bytes;
    for (const auto& x : kAreas) {
        INFO("area " << x.area);
        const auto* e = clockwork_arena_area(h, x.area);
        REQUIRE(e != nullptr);
        CHECK(e->space == CLOCKWORK_SPACE_ARENA);
        CHECK(e->base_lo == x.start);
        CHECK(e->base_lo + e->bytes_lo == x.end);
        CHECK(e->base_hi == 0); CHECK(e->bytes_hi == 0);
        CHECK(e->base_lo >= next);   // in order, no overlap
        next = e->base_lo + e->bytes_lo;
        if (x.guests) CHECK(e->base_lo >= h->guest_offset);
        else          CHECK(e->base_lo + e->bytes_lo <= h->clockwork_bytes);
    }
    CHECK(next == h->arena_bytes);   // and they cover it
    // The ten outside the arena are listed, with no place until the engine says.
    for (uint32_t id : { CLOCKWORK_AREA_ASSETS, CLOCKWORK_AREA_RETURNS, CLOCKWORK_AREA_GUEST_MEMORY_BULK, CLOCKWORK_AREA_GUEST_MEMORY_FAST,
                         CLOCKWORK_AREA_STATIC_DATA, CLOCKWORK_AREA_STACK, CLOCKWORK_AREA_MALLOC, CLOCKWORK_AREA_RUST_HEAP,
                         CLOCKWORK_AREA_ENGINE_MEMORY }) {
        INFO("area " << id);
        const auto* e = clockwork_arena_area(h, id);
        REQUIRE(e != nullptr);
        CHECK(e->space == CLOCKWORK_SPACE_UNKNOWN);
    }
}

TEST_CASE("arena: every region is where the constants put it, with its five facts", "[arena]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    CHECK(h->entry_count == std::size(kExpect));
    for (const auto& x : kExpect) {
        INFO("region " << x.id);
        const auto* e = entry(a, x.id);
        REQUIRE(e != nullptr);
        CHECK(e->offset == x.start);
        CHECK(e->bytes  == x.size);
        CHECK(e->owner  == x.owner);
        CHECK(e->audience == x.audience);
        CHECK(e->cadence  == x.cadence);
        CHECK(e->tier     == x.tier);
        CHECK(e->lifetime == x.lifetime);
        CHECK(clockwork_arena_area_of(h, e) == x.area);
        // Inside the arena, and never inside the header.
        CHECK(e->offset >= h->header_bytes);
        CHECK(e->offset + e->bytes <= h->arena_bytes);
    }
    // The regions are a partition: sorted by offset, none overlaps the next.
    // The ids are the stable numbers 1..N, each once — a region added later
    // keeps the next number whatever its place in memory (the control ring,
    // 17, lies among the rings), so the set is pinned and not the order.
    std::vector<const ClockworkArenaEntry*> es;
    for (uint32_t i = 0; i < h->entry_count; ++i) es.push_back(&h->entries[i]);
    std::sort(es.begin(), es.end(), [](auto* p, auto* q) { return p->offset < q->offset; });
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < es.size(); ++i) {
        INFO("region " << es[i]->id);
        ids.push_back(es[i]->id);
        if (i > 0) CHECK(es[i-1]->offset + es[i-1]->bytes <= es[i]->offset);
    }
    std::sort(ids.begin(), ids.end());
    for (size_t i = 0; i < ids.size(); ++i) CHECK(ids[i] == i + 1);
}

TEST_CASE("arena: the geometry a reader walks by is in the table", "[arena]") {
    auto a = arena();
    const auto* in = entry(a, CLOCKWORK_ARENA_CLOCKWORK_IN_RING);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_MAX_MESSAGE]    == MAX_MESSAGE_SIZE);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_MESSAGE_MAGIC]  == MESSAGE_MAGIC);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_PADDING_MAGIC]  == PADDING_MAGIC);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_PADDING_MARKER] == RING_PADDING_MARKER);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_HEADER_BYTES]   == sizeof(Message));
    const auto* scope = entry(a, CLOCKWORK_ARENA_CLOCKWORK_SCOPE);
    CHECK(scope->geom[CLOCKWORK_GEOM_SCOPE_SLOTS]      == SCOPE_SLOTS);
    CHECK(scope->geom[CLOCKWORK_GEOM_SCOPE_SLOTS]      > CLOCKWORK_TAP_IN);
    CHECK(scope->geom[CLOCKWORK_GEOM_SCOPE_SLOT_BYTES] == SCOPE_SLOT_SIZE);
    CHECK(scope->geom[CLOCKWORK_GEOM_SCOPE_FRAMES]     == SCOPE_FRAMES);
    CHECK(scope->geom[CLOCKWORK_GEOM_SCOPE_CHANNELS]   == SCOPE_CHANNELS);
    // One tap region: the guest's slots, then the track slots, numbered after them.
    const auto* taps = entry(a, CLOCKWORK_ARENA_CLOCKWORK_TAPS);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAP_SLOTS]        == TAPS_SLOTS);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAP_SLOT_BYTES]   == TAP_SLOT_SIZE);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAP_RING_FRAMES]  == TAP_RING_FRAMES);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAP_TRACK_SLOTS]  == TAP_TRACK_SLOTS);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAP_TRACK_FIRST]  == TAPS_SLOTS);
    CHECK(taps->bytes == TAPS_HEADER_SIZE + (TAPS_SLOTS + TAP_TRACK_SLOTS) * TAP_SLOT_SIZE);
    const auto* anchors = entry(a, CLOCKWORK_ARENA_CLOCKWORK_CLOCK_ANCHORS);
    CHECK(anchors->offset + anchors->geom[CLOCKWORK_GEOM_ANCHOR_NTP_START] == NTP_START_TIME_START);
    CHECK(anchors->offset + anchors->geom[CLOCKWORK_GEOM_ANCHOR_DRIFT]     == DRIFT_OFFSET_START);
    CHECK(anchors->offset + anchors->geom[CLOCKWORK_GEOM_ANCHOR_GLOBAL]    == GLOBAL_OFFSET_START);
}

TEST_CASE("arena: one tap slot index space, the guest's slots then the track taps", "[arena]") {
    auto a = arena();
    ShmReaderLayout L;
    const char* why = "";
    REQUIRE(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
    CHECK(L.tapSlotCount() == TAPS_SLOTS + TAP_TRACK_SLOTS);
    const uint8_t* first = L.tapSlotAt(a.data(), 0);
    REQUIRE(first != nullptr);
    CHECK(first - a.data() == TAPS_START + TAPS_HEADER_SIZE);
    const uint8_t* lastGuest = L.tapSlotAt(a.data(), TAPS_SLOTS - 1);
    REQUIRE(lastGuest != nullptr);
    if (TAP_TRACK_SLOTS > 0) {
        const uint8_t* firstTrack = L.tapSlotAt(a.data(), TAPS_SLOTS);
        REQUIRE(firstTrack != nullptr);
        CHECK(firstTrack - lastGuest == TAP_SLOT_SIZE);   // the next slot, no gap
        CHECK(firstTrack - a.data() + TAP_SLOT_SIZE <= CLOCKWORK_HALF_SIZE);
    }
    CHECK(L.tapSlotAt(a.data(), L.tapSlotCount()) == nullptr);
}

TEST_CASE("arena: a reader's table round-trips through the header", "[arena]") {
    auto a = arena();
    const ShmReaderLayout mine = ShmReaderLayout::from_constants();
    ShmReaderLayout theirs;
    const char* why = "";
    REQUIRE(ShmReaderLayout::from_arena(a.data(), a.size(), theirs, &why));
    CHECK(std::memcmp(&mine, &theirs, sizeof mine) == 0);
}

TEST_CASE("arena: a reader refuses what it cannot use, and says why", "[arena]") {
    auto a = arena();
    auto* h = reinterpret_cast<ClockworkArenaHeader*>(a.data());
    ShmReaderLayout L;
    const char* why = "";
    SECTION("memory too small for the arena it claims") {
        CHECK_FALSE(ShmReaderLayout::from_arena(a.data(), ARENA_SIZE - 1, L, &why));
        CHECK(std::string(why) == "arena larger than the memory it is in");
    }
    SECTION("a version this reader was not written for") {
        h->version = CLOCKWORK_ARENA_VERSION + 1;
        CHECK_FALSE(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
        CHECK(std::string(why) == "arena version this reader does not know");
    }
    SECTION("not yet published") {
        h->state = CLOCKWORK_ARENA_LAYING_OUT;
        CHECK_FALSE(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
        CHECK(std::string(why) == "arena not yet published");
    }
    SECTION("a region the reader needs is missing") {
        // Drop the metrics entry by giving it another id.
        mutable_entry(a, CLOCKWORK_ARENA_CLOCKWORK_METRICS)->id = 999;
        CHECK_FALSE(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
        CHECK(std::string(why) == "arena has no metrics");
    }
    SECTION("an id it does not know is not an error") {
        // A region from a newer engine, in the state area and public, with
        // its facts stated: a reader that does not know the id walks past it.
        ClockworkArenaEntry e{ 999, h->header_bytes, 16, CLOCKWORK_OWNER_CLOCKWORK, CLOCKWORK_AUDIENCE_PUBLIC,
                               CLOCKWORK_CADENCE_BOOT, CLOCKWORK_TIER_FAST, CLOCKWORK_LIFETIME_ARENA, {} };
        h->entries[h->entry_count++] = e;
        CHECK(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
    }
}

// ── The areas and the facts ──────────────────────────────────────────────────
// Owner says who writes a region; the audience says who it is for; the area
// it lies in says the same thing for the whole run of memory, so a tool
// colours a map from the area table alone and the check refuses a table
// where the two disagree.

TEST_CASE("arena: what a client may read by hand is public, and only that", "[arena][facts]") {
    auto a = arena();
    for (uint32_t id : { CLOCKWORK_ARENA_CLOCKWORK_METRICS, CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS, CLOCKWORK_ARENA_CLOCKWORK_CLOCK_ANCHORS,
                         CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE, CLOCKWORK_ARENA_CLOCKWORK_SAMPLE_CLOCK, CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP,
                         CLOCKWORK_ARENA_CLOCKWORK_SCOPE, CLOCKWORK_ARENA_CLOCKWORK_TAPS, CLOCKWORK_ARENA_GUEST_WINDOW }) {
        INFO("region " << id);
        CHECK(clockwork_arena_public(arenaHeader(a.data()), id));
    }
    for (uint32_t id : { CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS, CLOCKWORK_ARENA_CLOCKWORK_NODE_ID_COUNTER, CLOCKWORK_ARENA_CLOCKWORK_IN_RING,
                         CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING, CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING,
                         CLOCKWORK_ARENA_GUEST_CONFIG, CLOCKWORK_ARENA_GUEST_PERSIST }) {
        INFO("region " << id);
        CHECK_FALSE(clockwork_arena_public(arenaHeader(a.data()), id));
    }
    // What the guest is handed read-only pointers to says so.
    CHECK(entry(a, CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE)->audience & CLOCKWORK_AUDIENCE_GUEST);
    CHECK(entry(a, CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP)->audience & CLOCKWORK_AUDIENCE_GUEST);
    CHECK_FALSE(entry(a, CLOCKWORK_ARENA_CLOCKWORK_IN_RING)->audience & CLOCKWORK_AUDIENCE_GUEST);
    // The node id counter is the clients', not clockwork's.
    CHECK(entry(a, CLOCKWORK_ARENA_CLOCKWORK_NODE_ID_COUNTER)->owner == CLOCKWORK_OWNER_CLIENT);
    // An id the arena has not got is not public.
    CHECK_FALSE(clockwork_arena_public(arenaHeader(a.data()), 999));
}

TEST_CASE("arena: tier follows cadence", "[arena][facts]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    for (uint32_t i = 0; i < h->entry_count; ++i) {
        const auto& e = h->entries[i];
        INFO("region " << e.id);
        // Everything touched every block wants fast RAM; the taps are the one
        // exception, too big to be anything but bulk on a tiered device.
        if (e.cadence == CLOCKWORK_CADENCE_BLOCK && e.id != CLOCKWORK_ARENA_CLOCKWORK_TAPS) CHECK(e.tier == CLOCKWORK_TIER_FAST);
        if (e.cadence == CLOCKWORK_CADENCE_BOOT) CHECK(e.tier == CLOCKWORK_TIER_BULK);
    }
}

TEST_CASE("arena: the check refuses a table whose areas and facts disagree", "[arena][facts]") {
    auto a = arena();
    auto* h = reinterpret_cast<ClockworkArenaHeader*>(a.data());
    const char* why = "";
    SECTION("a public region in the transport area") {
        mutable_entry(a, CLOCKWORK_ARENA_CLOCKWORK_IN_RING)->audience = CLOCKWORK_AUDIENCE_PUBLIC;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "a public region outside the public areas");
    }
    SECTION("a non-public region in a public area") {
        mutable_entry(a, CLOCKWORK_ARENA_CLOCKWORK_METRICS)->audience = CLOCKWORK_AUDIENCE_TRANSPORT;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "a non-public region in a public area");
    }
    SECTION("a region that lies in no area") {
        mutable_entry(a, CLOCKWORK_ARENA_CLOCKWORK_SCOPE)->offset = CLOCKWORK_AUDIO_START - 16;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "a region in no area");
    }
    SECTION("an area in the wrong half") {
        const_cast<ClockworkArenaAreaEntry*>(clockwork_arena_area(h, CLOCKWORK_AREA_GUEST_PUBLIC))->base_lo = h->guest_offset - 16;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "an area in the wrong half");
    }
    SECTION("two areas that overlap") {
        const_cast<ClockworkArenaAreaEntry*>(clockwork_arena_area(h, CLOCKWORK_AREA_CLOCKWORK_AUDIO))->base_lo -= 16;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "two areas overlap");
    }
    SECTION("a fact left unsaid") {
        mutable_entry(a, CLOCKWORK_ARENA_CLOCKWORK_METRICS)->lifetime = 0;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "a region with no lifetime");
    }
    SECTION("an area of the arena placed outside it") {
        const_cast<ClockworkArenaAreaEntry*>(clockwork_arena_area(h, CLOCKWORK_AREA_GUEST_PRIVATE))->space = CLOCKWORK_SPACE_PROCESS;
        CHECK_FALSE(clockwork_arena_check(h, ARENA_SIZE, &why));
        CHECK(std::string(why) == "an area of the arena placed outside it");
    }
}

TEST_CASE("arena: the three areas outside it are placed when the engine knows where", "[arena][facts]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    const char* why = "";
    CHECK(clockwork_arena_area(h, CLOCKWORK_AREA_ASSETS)->space == CLOCKWORK_SPACE_UNKNOWN);
    arenaSetArea(a.data(), CLOCKWORK_AREA_ASSETS, CLOCKWORK_SPACE_PROCESS, 0x1234'5678'9ABCull, 8u << 20);
    arenaSetArea(a.data(), CLOCKWORK_AREA_GUEST_MEMORY_BULK, CLOCKWORK_SPACE_PROCESS, 0x7000'0000ull, 64u << 20);
    const auto* assets = clockwork_arena_area(h, CLOCKWORK_AREA_ASSETS);
    CHECK(assets->space == CLOCKWORK_SPACE_PROCESS);
    CHECK((uint64_t(assets->base_hi) << 32 | assets->base_lo) == 0x1234'5678'9ABCull);
    CHECK((uint64_t(assets->bytes_hi) << 32 | assets->bytes_lo) == (8u << 20));
    CHECK(clockwork_arena_area(h, CLOCKWORK_AREA_RETURNS)->space == CLOCKWORK_SPACE_UNKNOWN);
    // Nothing else moved: the table is still the same contract.
    CHECK(clockwork_arena_check(h, ARENA_SIZE, &why));
    // An area the table does not list is not written.
    arenaSetArea(a.data(), 99, CLOCKWORK_SPACE_PROCESS, 1, 1);
    CHECK(clockwork_arena_check(h, ARENA_SIZE, &why));
}

TEST_CASE("arena: the guest window's identity is unknown until a guest declares it", "[arena][facts]") {
    auto a = arena();
    const auto* w = entry(a, CLOCKWORK_ARENA_GUEST_WINDOW);
    CHECK(w->geom[CLOCKWORK_GEOM_WINDOW_MAGIC]   == 0);
    CHECK(w->geom[CLOCKWORK_GEOM_WINDOW_VERSION] == 0);
    // The engine writes what dsp_describe() declares when it binds the guest.
    arenaSetGuestWindowIdentity(a.data(), 0x44554D59u, 3u);
    CHECK(w->geom[CLOCKWORK_GEOM_WINDOW_MAGIC]   == 0x44554D59u);
    CHECK(w->geom[CLOCKWORK_GEOM_WINDOW_VERSION] == 3u);
    // Nothing else moved: the table is still the same contract.
    const char* why = "";
    CHECK(clockwork_arena_check(arenaHeader(a.data()), ARENA_SIZE, &why));
    CHECK(w->audience == CLOCKWORK_AUDIENCE_PUBLIC);
}
