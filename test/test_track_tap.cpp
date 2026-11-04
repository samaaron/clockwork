// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_track_tap.cpp — a live track's return has a tap stream (TrackControl::
 * tapReturns): the slot after the guest's, claimed in the core's arena the
 * block after the bridge says the track is there, and given back when it is
 * not. The bridge's word is its segment header's `slots_live`; this case
 * maps the segment as the bridge would and says it there.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "native/TrackControl.h"
#include "plugin_bridge.h"
#include "shm_segment.hpp"
#include "shared_memory.h"
#include "core_state.h"
#include "clockwork_core.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
int ownPid() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// The bridge's view of the engine's segment, for the length of a case.
struct BridgeSide {
    detail_shm_segment::shm_handle seg{};
    clockwork_bridge::Header* h = nullptr;
    explicit BridgeSide(ClockworkCore* core) {
        seg = detail_shm_segment::shm_open_existing(
            clockwork_bridge::segment_name(ownPid(), clockwork_core_id(core)));
        REQUIRE(seg.ptr != nullptr);
        h = clockwork_bridge::header(seg.ptr);
    }
    ~BridgeSide() { detail_shm_segment::shm_close(seg); }
};

uint32_t tapState(ClockworkCore* core, uint32_t slot) {
    const clockwork::Layout& L = clockwork::layout_of(core);
    REQUIRE(L.profile.track_tap_slots > slot);
    const shm_tap_stream* s = clockwork::tapSlot(clockwork::arena_base(core), L, L.profile.tap_slots + slot);
    return s->state.load(std::memory_order_acquire);
}
}

TEST_CASE("track tap: a live track's return is tapped in the slot after the guest's, and given back when it goes",
          "[tracks][taps][engine]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.reservedLanes = TrackControl::kLanes;
    EngineFixture fx(cfg);
    ClockworkCore* const core = fx.engine().core();
    REQUIRE(core != nullptr);
    BridgeSide bridge(core);

    fx.pumpBlock(2);
    CHECK(tapState(core, 0) == 0);

    // The bridge says slot 0 holds a track: the next return block claims
    // its tap.
    bridge.h->slots_live.store(1u, std::memory_order_release);
    fx.pumpBlock(2);
    CHECK(tapState(core, 0) == 1);
    CHECK(tapState(core, 1) == 0);

    // And gives it back when the track goes.
    bridge.h->slots_live.store(0u, std::memory_order_release);
    fx.pumpBlock(2);
    CHECK(tapState(core, 0) == 0);
}
