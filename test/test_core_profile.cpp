// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_core_profile.cpp — a core's own sizes (docs/ENGINES.md): a
 * host lays a core's arena out from a profile before its first init, the
 * core's table says so, and its clients find every region where it put
 * them.
 *
 * Here, in clockwork_attach_tests, because each case makes its cores and
 * closes them.
 */
#include "BootedCore.h"
#include "clockwork_client.h"
#include "clockwork_embed.h"
#include "shared_memory.h"
#include "OscTestUtils.h"
#include "ResidentMemory.h"
#include "Sanitizers.h"
#include "audio_processor.h"   // destroy_dsp, rebuild_dsp

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using namespace core_test;

ClockworkProfile defaults() {
    ClockworkProfile p {};
    p.struct_bytes = sizeof p;
    clockwork_profile_default(&p);
    return p;
}

// Smaller in everything a profile chooses than any target's own.
ClockworkProfile small() {
    ClockworkProfile p = defaults();
    p.in_ring_bytes      = 16 * 1024;
    p.rt_out_ring_bytes  = 8 * 1024;
    p.nrt_out_ring_bytes = 8 * 1024;
    p.scope_frames       = 4096;
    p.scope_channels     = 2;
    p.tap_slots          = 2;
    p.track_tap_slots    = 0;
    p.tap_ring_frames    = 4096;
    p.window_bytes       = 4096;
    p.persist_bytes      = 4096;
    return p;
}

// A client over a core's arena, as a host in its process opens one.
struct Client {
    ClockworkClient* c = nullptr;
    explicit Client(const Core& core) {
        ClockworkStatus st = CLOCKWORK_OK;
        c = clockwork_client_open_memory(clockwork_lanes_base(core.core), core.header()->arena_bytes, &st);
        REQUIRE(c != nullptr);
    }
    ~Client() { clockwork_client_close(c); }
    uint32_t bytes(ClockworkRegionId id) const {
        ClockworkRegion r {};
        REQUIRE(clockwork_client_region(c, id, &r) == CLOCKWORK_OK);
        return r.bytes;
    }
};
}

TEST_CASE("profile: the target's own sizes are the ones a core is laid out from without one", "[attach][profile]") {
    const ClockworkProfile p = defaults();
    const clockwork::Profile& own = clockwork::kDefaultProfile;
    CHECK(p.in_ring_bytes == own.in_ring_bytes);
    CHECK(p.rt_out_ring_bytes == own.rt_out_ring_bytes);
    CHECK(p.nrt_out_ring_bytes == own.nrt_out_ring_bytes);
    CHECK(p.scope_frames == own.scope_frames);
    CHECK(p.scope_channels == own.scope_channels);
    CHECK(p.tap_slots == own.tap_slots);
    CHECK(p.track_tap_slots == own.track_tap_slots);
    CHECK(p.tap_ring_frames == own.tap_ring_frames);
    CHECK(p.window_bytes == own.window_bytes);
    CHECK(p.persist_bytes == own.persist_bytes);
    Core plain;
    CHECK(plain.header()->arena_bytes == clockwork::kDefaultLayout.arena_bytes);
}

TEST_CASE("profile: a core laid out from its own says so in its table, and its clients find every region there",
          "[attach][profile]") {
    const ClockworkProfile p = small();
    Core mine(&p), beside;
    const clockwork::Layout L = clockwork::layOut(clockwork::Profile{
        p.in_ring_bytes, p.rt_out_ring_bytes, p.nrt_out_ring_bytes, p.scope_frames, p.scope_channels,
        p.tap_slots, p.track_tap_slots, p.tap_ring_frames, p.window_bytes, p.persist_bytes });
    REQUIRE(L.refused == nullptr);
    CHECK(mine.header()->arena_bytes == L.arena_bytes);
    CHECK(mine.header()->arena_bytes < beside.header()->arena_bytes / 8);
    CHECK(mine.region(CLOCKWORK_ARENA_CLOCKWORK_IN_RING).bytes == p.in_ring_bytes);
    CHECK(mine.region(CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING).bytes == p.rt_out_ring_bytes);
    CHECK(mine.region(CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING).bytes == p.nrt_out_ring_bytes);
    CHECK(mine.region(CLOCKWORK_ARENA_CLOCKWORK_SCOPE).geom[CLOCKWORK_GEOM_SCOPE_FRAMES] == p.scope_frames);
    CHECK(mine.region(CLOCKWORK_ARENA_CLOCKWORK_TAPS).geom[CLOCKWORK_GEOM_TAP_SLOTS] == p.tap_slots);
    CHECK(mine.region(CLOCKWORK_ARENA_CLOCKWORK_TAPS).geom[CLOCKWORK_GEOM_TAP_RING_FRAMES] == p.tap_ring_frames);
    CHECK(mine.region(CLOCKWORK_ARENA_GUEST_WINDOW).bytes == p.window_bytes);
    CHECK(mine.region(CLOCKWORK_ARENA_GUEST_PERSIST).bytes == p.persist_bytes);
    CHECK(beside.region(CLOCKWORK_ARENA_CLOCKWORK_IN_RING).bytes == clockwork::kDefaultProfile.in_ring_bytes);

    // Its client reads the same, and the core renders with it.
    Client client(mine);
    CHECK(client.bytes(CLOCKWORK_REGION_SCOPE) == L.scope_bytes);
    ClockworkScopeShape shape {};
    shape.struct_bytes = sizeof shape;
    REQUIRE(clockwork_client_scope_shape(client.c, &shape) == CLOCKWORK_OK);
    CHECK(shape.slots == SCOPE_SLOTS);
    CHECK(shape.slot_bytes == L.scope_slot_bytes);
    CHECK(shape.header_bytes == SCOPE_HEADER_SIZE);
    CHECK(shape.frames == p.scope_frames);
    CHECK(shape.channels == p.scope_channels);
    CHECK(client.bytes(CLOCKWORK_REGION_INGRESS) == p.in_ring_bytes);
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    CHECK(clockwork_tick(mine.core, &time, 2, 2) == CLOCKWORK_TICK_RENDERED);

    // A message its ring can never carry is too big for it, and only for it.
    std::vector<uint8_t> bytes(20 * 1024, 7);
    const auto big = osc_test::messageWithBlob("/dummy/blob", bytes.data(), bytes.size());
    CHECK(clockwork_client_send(client.c, big.ptr(), big.size(), 1) == CLOCKWORK_E_TOO_BIG);
    Client other(beside);
    CHECK(clockwork_client_send(other.c, big.ptr(), big.size(), 1) == CLOCKWORK_OK);
}

TEST_CASE("profile: one no arena can hold is refused with the reason, and the core keeps its own sizes",
          "[attach][profile]") {
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkCore* core = clockwork_core_new(&st);
    REQUIRE(core != nullptr);
    ClockworkProfile tiny = small();
    tiny.in_ring_bytes = 1000;
    const char* why = clockwork_set_profile(core, &tiny);
    REQUIRE(why != nullptr);
    CHECK(std::string_view(why).find("4 KB") != std::string_view::npos);
    ClockworkProfile shortScope = small();
    shortScope.scope_frames = 16;
    why = clockwork_set_profile(core, &shortScope);
    REQUIRE(why != nullptr);
    CHECK(std::string_view(why).find("largest block") != std::string_view::npos);
    ClockworkProfile crowded = small();
    crowded.tap_slots = 60;
    crowded.track_tap_slots = 8;
    why = clockwork_set_profile(core, &crowded);
    REQUIRE(why != nullptr);
    CHECK(std::string_view(why).find("tap slots") != std::string_view::npos);
    ClockworkProfile old = small();
    old.struct_bytes = 8;
    CHECK(clockwork_set_profile(core, &old) != nullptr);
    CHECK(clockwork_set_profile(core, nullptr) != nullptr);
    clockwork_core_close(core);

    // A core laid out already keeps what it was laid out from.
    Core plain;
    const ClockworkProfile p = small();
    why = clockwork_set_profile(plain.core, &p);
    REQUIRE(why != nullptr);
    CHECK(std::string_view(why).find("laid out already") != std::string_view::npos);
    CHECK(plain.header()->arena_bytes == clockwork::kDefaultLayout.arena_bytes);
}



// An engine brought up through the embed door names its profile in its
// config; one no arena can hold is refused before anything is made.
TEST_CASE("profile: an attached engine is laid out from the profile its config names", "[attach][profile][embed]") {
    ClockworkProfile plugin {};
    plugin.struct_bytes = sizeof plugin;
    clockwork_profile_plugin(&plugin);
    REQUIRE(clockwork_profile_check(&plugin) == nullptr);
    ClockworkEmbedConfig cfg {};
    cfg.struct_bytes    = sizeof cfg;
    cfg.sample_rate     = kRate;
    cfg.output_channels = 2;
    cfg.profile         = &plugin;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkEmbed* small = clockwork_embed_attach(&cfg, &st);
    REQUIRE(small != nullptr);
    cfg.profile = nullptr;
    ClockworkEmbed* big = clockwork_embed_attach(&cfg, &st);
    REQUIRE(big != nullptr);
    const auto ingress = [](ClockworkEmbed* h) {
        ClockworkRegion r {};
        REQUIRE(clockwork_client_region(clockwork_embed_client(h), CLOCKWORK_REGION_INGRESS, &r) == CLOCKWORK_OK);
        return r.bytes;
    };
    CHECK(ingress(small) == plugin.in_ring_bytes);
    CHECK(ingress(big) == clockwork::kDefaultProfile.in_ring_bytes);
    clockwork_embed_close(small);
    clockwork_embed_close(big);

    ClockworkProfile tiny = plugin;
    tiny.scope_frames = 16;
    CHECK(clockwork_profile_check(&tiny) != nullptr);
    cfg.profile = &tiny;
    CHECK(clockwork_embed_attach(&cfg, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ARG);
}

// The budget (docs/ENGINES.md): 32 cores of the plugin profile
// in one process, each with the target's guest bulk span (64 MB natively),
// in 128 MB resident. The guest's spans are zeroed at every build
// (dsp_api.h) by giving their pages back, so a core costs what its guest
// touches of them rather than what they reserve; written whole, each core's
// bulk span alone was 64 MB.
TEST_CASE("profile: 32 plugin cores fit the budget, each costing what its guest touches", "[attach][profile][memory]") {
    ClockworkProfile plugin {};
    plugin.struct_bytes = sizeof plugin;
    clockwork_profile_plugin(&plugin);
    { Core warm(&plugin); }
    const size_t before = test_memory::residentBytes();
    if (before == 0) SKIP("this platform does not say how much of the process is resident");
    if (CLOCKWORK_UNDER_ASAN || CLOCKWORK_UNDER_TSAN) SKIP("a sanitizer's shadow memory is resident too");
    constexpr int kCores = 32;
    constexpr size_t kBudget = 128u << 20;
    std::vector<std::unique_ptr<Core>> cores;
    for (int i = 0; i < kCores; ++i) {
        cores.push_back(std::make_unique<Core>(&plugin));
        ClockworkBlockTime time {};
        time.struct_bytes = sizeof time;
        for (int b = 0; b < 8; ++b) clockwork_tick(cores.back()->core, &time, 2, 2);
    }
    const size_t after = test_memory::residentBytes();
    INFO("resident: " << ((after - before) >> 20) << " MB for " << kCores << " cores");
    CHECK(after - before < kBudget);
}

// What the last instance wrote is gone at the next build: the promise the
// span's zeroing keeps, however it is kept. The middle of the bulk span, far
// from anything the placeholder's few allocations reach.
TEST_CASE("profile: a rebuilt guest's bulk span holds nothing the last instance wrote", "[attach][profile][memory]") {
    Core c;
    const auto span = [&](uint32_t id) {
        const ClockworkArenaAreaEntry* a = clockwork_arena_area(c.header(), id);
        REQUIRE(a != nullptr);
        const uint64_t base  = uint64_t(a->base_lo) | (uint64_t(a->base_hi) << 32);
        const uint64_t bytes = uint64_t(a->bytes_lo) | (uint64_t(a->bytes_hi) << 32);
        REQUIRE(bytes >= (4u << 20));
        return std::pair<uint8_t*, size_t>{ reinterpret_cast<uint8_t*>(uintptr_t(base)), size_t(bytes) };
    };
    const auto [bulk, bytes] = span(CLOCKWORK_AREA_GUEST_MEMORY_BULK);
    uint8_t* const middle = bulk + bytes / 2;
    constexpr size_t kMiddle = 1u << 20;
    std::memset(middle, 0xA5, kMiddle);
    destroy_dsp(c.core);
    rebuild_dsp(c.core, kRate, kBlock);
    REQUIRE(span(CLOCKWORK_AREA_GUEST_MEMORY_BULK).first == bulk);
    size_t written = 0;
    for (size_t i = 0; i < kMiddle; ++i) written += middle[i] != 0;
    CHECK(written == 0);
}

TEST_CASE("profile: a core's arena names the core in its header", "[attach][profile]") {
    Core a, b;
    CHECK(a.header()->instance_id == clockwork_core_id(a.core));
    CHECK(b.header()->instance_id == clockwork_core_id(b.core));
    CHECK(a.header()->instance_id != b.header()->instance_id);
}
