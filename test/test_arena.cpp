// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_arena.cpp — the arena's own table (clockwork_arena.h).
 *
 * The constants in shared_memory.h lay the arena out; writeArenaHeader turns
 * them into the table every reader takes the layout from. These pin the
 * contract: the table describes the constants exactly, every region lies
 * inside the arena and inside the half its owner is allowed, the two halves
 * do not overlap, and a reader refuses what it cannot use.
 */
#include <catch2/catch_test_macros.hpp>
#include "shared_memory.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::vector<uint8_t> arena() {
    std::vector<uint8_t> a(TOTAL_BUFFER_SIZE, 0);
    writeArenaHeader(a.data());
    return a;
}
const ClockworkArenaEntry* entry(const std::vector<uint8_t>& a, uint32_t id) {
    return clockwork_arena_find(arenaHeader(a.data()), id);
}
}

TEST_CASE("arena: the header is the front of the arena and says so", "[arena]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    CHECK(h->magic == CLOCKWORK_ARENA_MAGIC);
    CHECK(h->version == CLOCKWORK_ARENA_VERSION);
    CHECK(h->state == CLOCKWORK_ARENA_PUBLISHED);
    CHECK(h->header_bytes == ARENA_HEADER_SIZE);
    CHECK(h->arena_bytes == TOTAL_BUFFER_SIZE);
    CHECK(h->entry_bytes == sizeof(ClockworkArenaEntry));
    CHECK(sizeof(ClockworkArenaEntry) == 64);
    CHECK(sizeof(ClockworkArenaHeader) <= ARENA_HEADER_SIZE);
    // The two halves, edge to edge, covering the whole arena.
    CHECK(h->block_bytes == CLOCKWORK_BLOCK_SIZE);
    CHECK(h->guest_offset == h->block_bytes);
    CHECK(h->guest_offset + h->guest_bytes == h->arena_bytes);
    const char* why = "";
    CHECK(clockwork_arena_check(h, TOTAL_BUFFER_SIZE, &why));
}

namespace {
// Every region: where the constants put it, who writes it, and who it is for.
struct Expect { uint32_t id, start, size, owner, audience; };
constexpr uint32_t PUB = CLOCKWORK_AUDIENCE_PUBLISHED, GUEST = CLOCKWORK_AUDIENCE_GUEST,
                   HOST = CLOCKWORK_AUDIENCE_HOST, TRANSPORT = CLOCKWORK_AUDIENCE_TRANSPORT;
const Expect kExpect[] = {
    // the block's published run: a data contract for any reader
    { CLOCKWORK_ARENA_METRICS,         METRICS_START,         METRICS_SIZE,         CLOCKWORK_OWNER_CLOCKWORK, PUB },
    { CLOCKWORK_ARENA_NATIVE_STATS,    NATIVE_STATS_START,    NATIVE_STATS_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, PUB },
    { CLOCKWORK_ARENA_CLOCK_ANCHORS,   CLOCK_ANCHORS_START,   CLOCK_ANCHORS_SIZE,   CLOCKWORK_OWNER_HOST,      PUB },
    { CLOCKWORK_ARENA_CLOCK_STATE,     CLOCK_STATE_START,     CLOCK_STATE_SIZE,     CLOCKWORK_OWNER_CLOCKWORK, PUB | GUEST },
    { CLOCKWORK_ARENA_SAMPLE_CLOCK,    SAMPLE_CLOCK_START,    SAMPLE_CLOCK_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, PUB },
    { CLOCKWORK_ARENA_CHANNEL_MAP,     CHANNEL_MAP_START,     CHANNEL_MAP_SIZE,     CLOCKWORK_OWNER_CLOCKWORK, PUB | GUEST },
    { CLOCKWORK_ARENA_AUDIO_TAPS,      SHM_AUDIO_START,       SHM_AUDIO_TOTAL_SIZE, CLOCKWORK_OWNER_CLOCKWORK, PUB },
    { CLOCKWORK_ARENA_TRACK_TAPS,      SHM_TRACK_TAPS_START,  SHM_TRACK_TAPS_SIZE,  CLOCKWORK_OWNER_CLOCKWORK, PUB },
    // the block's transport run: the command plane, touched through the client ABI only
    { CLOCKWORK_ARENA_CONTROL,         CONTROL_START,         CONTROL_SIZE,         CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT },
    { CLOCKWORK_ARENA_NODE_ID_COUNTER, NODE_ID_COUNTER_START, NODE_ID_COUNTER_SIZE, CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT },
    { CLOCKWORK_ARENA_IN_RING,         IN_BUFFER_START,       IN_BUFFER_SIZE,       CLOCKWORK_OWNER_CLIENT,    TRANSPORT },
    { CLOCKWORK_ARENA_OUT_RING,        OUT_BUFFER_START,      OUT_BUFFER_SIZE,      CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT },
    { CLOCKWORK_ARENA_NRT_OUT_RING,    NRT_OUT_BUFFER_START,  NRT_OUT_BUFFER_SIZE,  CLOCKWORK_OWNER_CLOCKWORK, TRANSPORT },
    { CLOCKWORK_ARENA_CLIENT_SLOTS,    CLIENT_SLOTS_START,    CLIENT_SLOTS_SIZE,    CLOCKWORK_OWNER_CLIENT,    TRANSPORT },
    // the guest's published run: what the guest shows its clients
    { CLOCKWORK_ARENA_GUEST_WINDOW,    SHM_WINDOW_START,      SHM_WINDOW_SIZE,      CLOCKWORK_OWNER_GUEST,     PUB },
    { CLOCKWORK_ARENA_SCOPE,           SHM_SCOPE_START,       SHM_SCOPE_TOTAL_SIZE, CLOCKWORK_OWNER_GUEST,     PUB },
    // the guest's own: nobody else's business
    { CLOCKWORK_ARENA_GUEST_CONFIG,    GUEST_CONFIG_START,    GUEST_CONFIG_SIZE,    CLOCKWORK_OWNER_HOST,      GUEST | HOST },
    { CLOCKWORK_ARENA_GUEST_PERSIST,   GUEST_PERSIST_START,   GUEST_PERSIST_SIZE,   CLOCKWORK_OWNER_GUEST,     GUEST },
};
} // namespace

TEST_CASE("arena: every region is where the constants put it", "[arena]") {
    auto a = arena();
    const auto& expect = kExpect;
    const auto* h = arenaHeader(a.data());
    CHECK(h->entry_count == sizeof(expect) / sizeof(expect[0]));
    for (const auto& x : expect) {
        INFO("region " << x.id);
        const auto* e = entry(a, x.id);
        REQUIRE(e != nullptr);
        CHECK(e->offset == x.start);
        CHECK(e->bytes  == x.size);
        CHECK(e->owner  == x.owner);
        CHECK(clockwork_arena_audience(e) == x.audience);
        // Inside the arena, and never inside the header.
        CHECK(e->offset >= h->header_bytes);
        CHECK(e->offset + e->bytes <= h->arena_bytes);
        // The guest's regions are in the guest region; everything else is in
        // the block. That line is the whole point of the two halves.
        const bool guests = x.owner == CLOCKWORK_OWNER_GUEST || x.id == CLOCKWORK_ARENA_GUEST_CONFIG;
        if (guests) {
            CHECK(e->offset >= h->guest_offset);
        } else {
            CHECK(e->offset + e->bytes <= h->block_bytes);
        }
    }
    // The regions are a partition: sorted by offset, none overlaps the next.
    std::vector<const ClockworkArenaEntry*> es;
    for (uint32_t i = 0; i < h->entry_count; ++i) es.push_back(&h->entries[i]);
    std::sort(es.begin(), es.end(), [](auto* p, auto* q) { return p->offset < q->offset; });
    for (size_t i = 1; i < es.size(); ++i) {
        INFO("region " << es[i-1]->id << " then " << es[i]->id);
        CHECK(es[i-1]->offset + es[i-1]->bytes <= es[i]->offset);
    }
}

TEST_CASE("arena: the geometry a reader walks by is in the table", "[arena]") {
    auto a = arena();
    const auto* in = entry(a, CLOCKWORK_ARENA_IN_RING);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_MAX_MESSAGE]    == MAX_MESSAGE_SIZE);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_MESSAGE_MAGIC]  == MESSAGE_MAGIC);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_PADDING_MAGIC]  == PADDING_MAGIC);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_PADDING_MARKER] == RING_PADDING_MARKER);
    CHECK(in->geom[CLOCKWORK_GEOM_RING_HEADER_BYTES]   == sizeof(Message));
    const auto* taps = entry(a, CLOCKWORK_ARENA_AUDIO_TAPS);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAPS_SLOTS]      == SHM_AUDIO_SLOTS);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAPS_SLOTS]      > CLOCKWORK_TAP_IN);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAPS_SLOT_BYTES] == SHM_AUDIO_SLOT_SIZE);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAPS_FRAMES]     == SHM_AUDIO_FRAMES);
    CHECK(taps->geom[CLOCKWORK_GEOM_TAPS_CHANNELS]   == SHM_AUDIO_CHANNELS);
    const auto* sc = entry(a, CLOCKWORK_ARENA_SCOPE);
    CHECK(sc->geom[CLOCKWORK_GEOM_SCOPE_SLOTS]       == SHM_SCOPE_MAX_SCOPES);
    CHECK(sc->geom[CLOCKWORK_GEOM_SCOPE_SLOT_BYTES]  == SHM_SCOPE_SLOT_SIZE);
    CHECK(sc->geom[CLOCKWORK_GEOM_SCOPE_RING_FRAMES] == SHM_SCOPE_RING_FRAMES);
    const auto* anchors = entry(a, CLOCKWORK_ARENA_CLOCK_ANCHORS);
    CHECK(anchors->offset + anchors->geom[CLOCKWORK_GEOM_ANCHOR_NTP_START] == NTP_START_TIME_START);
    CHECK(anchors->offset + anchors->geom[CLOCKWORK_GEOM_ANCHOR_DRIFT]     == DRIFT_OFFSET_START);
    CHECK(anchors->offset + anchors->geom[CLOCKWORK_GEOM_ANCHOR_GLOBAL]    == GLOBAL_OFFSET_START);
    const auto* slots = entry(a, CLOCKWORK_ARENA_CLIENT_SLOTS);
    CHECK(slots->geom[CLOCKWORK_GEOM_SLOTS_COUNT]      == CLIENT_SLOT_COUNT);
    CHECK(slots->geom[CLOCKWORK_GEOM_SLOTS_SLOT_BYTES] == CLIENT_SLOT_SIZE);
    // The track taps are the same slot shape as the guest's, numbered after them.
    const auto* tr = entry(a, CLOCKWORK_ARENA_TRACK_TAPS);
    CHECK(tr->geom[CLOCKWORK_GEOM_TRACK_SLOTS]       == SHM_TRACK_TAPS_SLOTS);
    CHECK(tr->geom[CLOCKWORK_GEOM_TRACK_SLOT_BYTES]  == SHM_SCOPE_SLOT_SIZE);
    CHECK(tr->geom[CLOCKWORK_GEOM_TRACK_FIRST_INDEX] == SHM_SCOPE_MAX_SCOPES);
    CHECK(tr->bytes == SHM_TRACK_TAPS_SLOTS * SHM_SCOPE_SLOT_SIZE);
}

TEST_CASE("arena: one scope slot index space across the guest's slots and the track taps", "[arena]") {
    auto a = arena();
    ShmReaderLayout L;
    const char* why = "";
    REQUIRE(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
    CHECK(L.scopeSlotCount() == SHM_SCOPE_MAX_SCOPES + SHM_TRACK_TAPS_SLOTS);
    // The guest's last slot is in the guest region; the first track tap is in the block.
    const uint8_t* lastGuest = L.scopeSlotAt(a.data(), SHM_SCOPE_MAX_SCOPES - 1);
    REQUIRE(lastGuest != nullptr);
    CHECK(lastGuest - a.data() >= GUEST_REGION_START);
    if (SHM_TRACK_TAPS_SLOTS > 0) {
        const uint8_t* firstTrack = L.scopeSlotAt(a.data(), SHM_SCOPE_MAX_SCOPES);
        REQUIRE(firstTrack != nullptr);
        CHECK(firstTrack - a.data() == SHM_TRACK_TAPS_START);
        CHECK(firstTrack - a.data() + SHM_SCOPE_SLOT_SIZE <= CLOCKWORK_BLOCK_SIZE);
    }
    CHECK(L.scopeSlotAt(a.data(), L.scopeSlotCount()) == nullptr);
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
        CHECK_FALSE(ShmReaderLayout::from_arena(a.data(), TOTAL_BUFFER_SIZE - 1, L, &why));
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
        const_cast<ClockworkArenaEntry*>(clockwork_arena_find(h, CLOCKWORK_ARENA_METRICS))->id = 999;
        CHECK_FALSE(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
        CHECK(std::string(why) == "arena has no metrics");
    }
    SECTION("an id it does not know is not an error") {
        // A region from a newer engine, in the published run and marked so:
        // a reader that does not know the id walks past it.
        ClockworkArenaEntry e{ 999, h->header_bytes, 16, CLOCKWORK_OWNER_CLOCKWORK, {} };
        e.geom[CLOCKWORK_GEOM_AUDIENCE] = CLOCKWORK_AUDIENCE_PUBLISHED;
        h->entries[h->entry_count++] = e;
        CHECK(ShmReaderLayout::from_arena(a.data(), a.size(), L, &why));
    }
}

// ── Audiences ────────────────────────────────────────────────────────────────
// Owner says who writes a region; the audience word says who it is FOR. Each
// half is laid out as two runs — the published regions first, then the ones
// that are transport or private — and the header names the boundary, so a
// tool colours a map in two strokes and nothing a client reads by hand
// shares a cache line with a ring cursor.

TEST_CASE("arena: the audience word is the last geometry word of every entry", "[arena][audience]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    CHECK(CLOCKWORK_GEOM_AUDIENCE == CLOCKWORK_ARENA_GEOM_WORDS - 1);
    for (uint32_t i = 0; i < h->entry_count; ++i) {
        INFO("region " << h->entries[i].id);
        CHECK(h->entries[i].geom[CLOCKWORK_GEOM_AUDIENCE] != 0);   // nobody is for nobody
        CHECK(clockwork_arena_audience(&h->entries[i]) == h->entries[i].geom[CLOCKWORK_GEOM_AUDIENCE]);
    }
    // A region's own geometry never reaches the audience word.
    CHECK(CLOCKWORK_GEOM_SLOTS_STACK_BYTES < CLOCKWORK_GEOM_AUDIENCE);
    CHECK(CLOCKWORK_GEOM_TRACK_FIRST_INDEX < CLOCKWORK_GEOM_AUDIENCE);
    CHECK(CLOCKWORK_GEOM_SCOPE_CHANNELS   < CLOCKWORK_GEOM_AUDIENCE);
    CHECK(CLOCKWORK_GEOM_TAPS_SAMPLE_RATE < CLOCKWORK_GEOM_AUDIENCE);
}

TEST_CASE("arena: what a client may read by hand is published, and only that", "[arena][audience]") {
    auto a = arena();
    // The data contracts: read by the GUI, a spider, the web client, by hand.
    for (uint32_t id : { CLOCKWORK_ARENA_METRICS, CLOCKWORK_ARENA_NATIVE_STATS, CLOCKWORK_ARENA_CLOCK_ANCHORS,
                         CLOCKWORK_ARENA_CLOCK_STATE, CLOCKWORK_ARENA_SAMPLE_CLOCK, CLOCKWORK_ARENA_CHANNEL_MAP,
                         CLOCKWORK_ARENA_AUDIO_TAPS, CLOCKWORK_ARENA_TRACK_TAPS,
                         CLOCKWORK_ARENA_GUEST_WINDOW, CLOCKWORK_ARENA_SCOPE }) {
        INFO("region " << id);
        CHECK(clockwork_arena_published(arenaHeader(a.data()), id));
    }
    // The command plane: only the client ABI touches it. The guest's own: only the guest.
    for (uint32_t id : { CLOCKWORK_ARENA_CONTROL, CLOCKWORK_ARENA_NODE_ID_COUNTER, CLOCKWORK_ARENA_IN_RING,
                         CLOCKWORK_ARENA_OUT_RING, CLOCKWORK_ARENA_NRT_OUT_RING, CLOCKWORK_ARENA_CLIENT_SLOTS,
                         CLOCKWORK_ARENA_GUEST_CONFIG, CLOCKWORK_ARENA_GUEST_PERSIST }) {
        INFO("region " << id);
        CHECK_FALSE(clockwork_arena_published(arenaHeader(a.data()), id));
    }
    // What the guest is handed read-only pointers to says so.
    CHECK(clockwork_arena_audience(entry(a, CLOCKWORK_ARENA_CLOCK_STATE)) & CLOCKWORK_AUDIENCE_GUEST);
    CHECK(clockwork_arena_audience(entry(a, CLOCKWORK_ARENA_CHANNEL_MAP)) & CLOCKWORK_AUDIENCE_GUEST);
    CHECK_FALSE(clockwork_arena_audience(entry(a, CLOCKWORK_ARENA_IN_RING)) & CLOCKWORK_AUDIENCE_GUEST);
    // An id the arena has not got is not published.
    CHECK_FALSE(clockwork_arena_published(arenaHeader(a.data()), 999));
}

TEST_CASE("arena: each half is two contiguous runs, published first, and the header says where", "[arena][audience]") {
    auto a = arena();
    const auto* h = arenaHeader(a.data());
    REQUIRE(h->block_published_end != 0);
    REQUIRE(h->guest_published_end != 0);
    CHECK(h->block_published_end == CLOCKWORK_BLOCK_PUBLISHED_END);
    CHECK(h->guest_published_end == GUEST_PUBLISHED_END);
    CHECK(h->block_published_end >  h->header_bytes);
    CHECK(h->block_published_end <= h->block_bytes);
    CHECK(h->guest_published_end >  h->guest_offset);
    CHECK(h->guest_published_end <= h->arena_bytes);
    for (const auto& x : kExpect) {
        INFO("region " << x.id);
        const auto* e = entry(a, x.id);
        REQUIRE(e != nullptr);
        const bool published = (x.audience & CLOCKWORK_AUDIENCE_PUBLISHED) != 0;
        const bool in_guest  = e->offset >= h->guest_offset;
        const uint32_t end   = in_guest ? h->guest_published_end : h->block_published_end;
        if (published) CHECK(e->offset + e->bytes <= end);   // inside its half's published run
        else           CHECK(e->offset >= end);              // after it
    }
    // The published runs are the FRONT of each half: the first region of the
    // block and the first of the guest region are both published.
    CHECK(METRICS_START == h->header_bytes);
    CHECK(SHM_WINDOW_START == h->guest_offset);
}

TEST_CASE("arena: the check refuses a table whose runs and audiences disagree", "[arena][audience]") {
    auto a = arena();
    auto* h = reinterpret_cast<ClockworkArenaHeader*>(a.data());
    const char* why = "";
    SECTION("a published region past the published run") {
        // Mark a transport region as published without moving it.
        const_cast<ClockworkArenaEntry*>(clockwork_arena_find(h, CLOCKWORK_ARENA_IN_RING))
            ->geom[CLOCKWORK_GEOM_AUDIENCE] = CLOCKWORK_AUDIENCE_PUBLISHED;
        CHECK_FALSE(clockwork_arena_check(h, TOTAL_BUFFER_SIZE, &why));
        CHECK(std::string(why) == "a published region outside the published run");
    }
    SECTION("an unpublished region inside the published run") {
        const_cast<ClockworkArenaEntry*>(clockwork_arena_find(h, CLOCKWORK_ARENA_METRICS))
            ->geom[CLOCKWORK_GEOM_AUDIENCE] = CLOCKWORK_AUDIENCE_TRANSPORT;
        CHECK_FALSE(clockwork_arena_check(h, TOTAL_BUFFER_SIZE, &why));
        CHECK(std::string(why) == "an unpublished region inside the published run");
    }
    SECTION("a boundary outside its half") {
        h->block_published_end = h->block_bytes + 16;
        CHECK_FALSE(clockwork_arena_check(h, TOTAL_BUFFER_SIZE, &why));
        CHECK(std::string(why) == "the block's published run runs past the block");
    }
    SECTION("a guest boundary before the guest region") {
        h->guest_published_end = h->guest_offset - 16;
        CHECK_FALSE(clockwork_arena_check(h, TOTAL_BUFFER_SIZE, &why));
        CHECK(std::string(why) == "the guest's published run is outside the guest region");
    }
    SECTION("a writer that says nothing about runs is still readable") {
        // An older table: zero boundaries, zero audiences. Not a refusal —
        // the reader just cannot ask who a region is for.
        h->block_published_end = h->guest_published_end = 0;
        for (uint32_t i = 0; i < h->entry_count; ++i) h->entries[i].geom[CLOCKWORK_GEOM_AUDIENCE] = 0;
        CHECK(clockwork_arena_check(h, TOTAL_BUFFER_SIZE, &why));
        CHECK_FALSE(clockwork_arena_published(h, CLOCKWORK_ARENA_METRICS));
    }
}
