// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_core_gate.cpp — the core's gate (docs/ENGINES.md): one
 * thread at a time works on a core's guest and its pools. A block takes the
 * gate and never waits; a control call waits out a block in flight.
 *
 * Here, in clockwork_attach_tests, because each case makes its cores and
 * closes them.
 */
#include "BootedCore.h"
#include "dsp_api.h"
#include "clockwork_heap.h"
#include "mem_region.h"
#include "ResidentMemory.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>

namespace {
using namespace core_test;
using clockwork::GateHold;
using Take = clockwork::GateHold::Take;

// A block from a thread of its own, as a host's audio thread renders.
ClockworkTick tickElsewhere(ClockworkCore* core) {
    ClockworkTick r = CLOCKWORK_TICK_NONE;
    std::thread([&] { r = tickHere(core); }).join();
    return r;
}
}

TEST_CASE("gate: a block while a control call holds the gate renders nothing, says so, and is counted",
          "[attach][gate]") {
    Core c;
    REQUIRE(tickElsewhere(c.core) == CLOCKWORK_TICK_RENDERED);
    {
        const GateHold control(c.core, CLOCKWORK_ENTRY_CONTROL, Take::Wait);
        REQUIRE(control.held());
        CHECK(tickElsewhere(c.core) == CLOCKWORK_TICK_BUSY);
        CHECK(clockwork_core_gate_busy(c.core) == 1);
    }
    CHECK(tickElsewhere(c.core) == CLOCKWORK_TICK_RENDERED);
    CHECK(clockwork_core_gate_busy(c.core) == 1);
}

TEST_CASE("gate: a control call made during a block begins after it", "[attach][gate]") {
    Core c;
    std::optional<GateHold> block;
    block.emplace(c.core, CLOCKWORK_ENTRY_RENDER, Take::Try);   // a block in flight, on this thread
    REQUIRE(block->held());

    std::atomic<bool> began{false};
    std::thread control([&] {
        const GateHold hold(c.core, CLOCKWORK_ENTRY_CONTROL, Take::Wait);
        began = hold.held();
    });
    // Until the control call is waiting on the gate's word, or has gone on
    // without it: only a wait counts itself on the word.
    while (clockwork_core_gate_waiters(c.core) == 0 && !began.load()) std::this_thread::yield();
    CHECK_FALSE(began.load());

    block.reset();   // the block ends
    control.join();
    CHECK(began.load());
}

TEST_CASE("gate: the thread that holds the gate takes it again for nothing", "[attach][gate]") {
    Core c;
    const GateHold control(c.core, CLOCKWORK_ENTRY_CONTROL, Take::Wait);
    REQUIRE(control.held());
    CHECK(tickHere(c.core) == CLOCKWORK_TICK_RENDERED);
    {
        const GateHold again(c.core, CLOCKWORK_ENTRY_CONTROL, Take::Wait);
        CHECK(again.held());
    }
    CHECK(clockwork_core_gate_busy(c.core) == 0);
    // Given back once, by the scope that took it.
    CHECK(clockwork_core_gate_held(c.core));
}

// The guest's pool was the thread's that last said it was the engine's,
// and stayed that thread's whatever it did after: the thread that booted
// the core went on owning the pool until the first block took it over.
TEST_CASE("gate: a thread that built the core keeps no claim on the guest's pool", "[attach][gate]") {
    Core c;   // built on this thread
    const ClockworkHeap* heap = clockwork_core_heap(c.core);
    REQUIRE(clockwork_heap_running(heap, CLOCKWORK_POOL_GUEST));
    CHECK(clockwork_heap_take(heap, CLOCKWORK_POOL_GUEST, 64) == nullptr);

    const GateHold control(c.core, CLOCKWORK_ENTRY_CONTROL, Take::Wait);
    void* inside = clockwork_heap_take(heap, CLOCKWORK_POOL_GUEST, 64);
    REQUIRE(inside != nullptr);
    CHECK(clockwork_heap_holds(heap, CLOCKWORK_POOL_GUEST, inside));
    CHECK(clockwork_heap_give(heap, CLOCKWORK_POOL_GUEST, inside));
}

// Suspect 4 (docs/ENGINES.md): on the attach path nothing gave back the
// host's memory a block parked while the engine ran; only the native egress
// thread did. The next call into the core that is not a block gives it back.
TEST_CASE("gate: the host's memory a block parked goes back at the next control call", "[attach][gate]") {
    Core c;
    const ClockworkHeap* heap = clockwork_core_heap(c.core);
    void* p = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, 64);   // what a thread without the gate takes
    REQUIRE(p != nullptr);
    clockwork_heap_park_system(heap, p);                                // given back during a block
    { const GateHold control(c.core, CLOCKWORK_ENTRY_CONTROL, Take::Wait); }
    static int left;
    left = 0;
    clockwork_heap_release_system(heap, [](void* q) { ++left; clockwork::mem::free(q); });
    CHECK(left == 0);
}

// Nothing outlives its core (docs/ENGINES.md): a core's own pool
// is empty when the core goes, after its sinks and ports.
TEST_CASE("heap: a core goes with none of clockwork's own memory left behind", "[attach][heap]") {
    const uint32_t before = clockwork_heap_outlived();
    {
        Core c;
        const ClockworkHeap* heap = clockwork_core_heap(c.core);
        REQUIRE(clockwork_heap_running(heap, CLOCKWORK_POOL_OWN));
        CHECK(tickElsewhere(c.core) == CLOCKWORK_TICK_RENDERED);
        CHECK(clockwork_heap_census(heap, CLOCKWORK_POOL_OWN).fallbacks == 0);
    }
    CHECK(clockwork_heap_outlived() == before);
}

// A core gives its memory back as it goes: its arena, the block its guest's
// spans are cut from, its pools (docs/ENGINES.md.6). Each core here
// holds well over a hundred megabytes, so cores that kept theirs would leave
// the process growing by that much each.
TEST_CASE("heap: cores made and closed one after another give their memory back", "[attach][heap]") {
    { Core warm; tickHere(warm.core); }
    const size_t before = test_memory::residentBytes();
    if (before == 0) SKIP("this platform does not say how much of the process is resident");
    for (int i = 0; i < 16; ++i) {
        Core c;
        for (int b = 0; b < 4; ++b) tickHere(c.core);
    }
    const size_t after = test_memory::residentBytes();
    INFO("resident before " << (before >> 20) << " MB, after sixteen cores " << (after >> 20) << " MB");
    CHECK(after < before + (64u << 20));
    CHECK(clockwork_heap_outlived() == 0);
}

// The guest says how many instances one process may hold (DspInfo::instances,
// docs/BOUNDARY.md): the dummy keeps nothing process-wide and says any, and
// every core builds it. One that leaves the count unset is held once
// (test_guest_once.cpp).
TEST_CASE("guest: one that says any is built in every core", "[attach][guest]") {
    REQUIRE(dsp_describe()->instances == DSP_INSTANCES_ANY);
    Core a, b, c;
    for (const Core* core : { &a, &b, &c }) {
        CHECK(clockwork_boot_error(core->core) == nullptr);
        CHECK(tickHere(core->core) == CLOCKWORK_TICK_RENDERED);
    }
}

TEST_CASE("gate: a closed core's block is no block, and the host is told to stop", "[attach][gate]") {
    ClockworkCore* closed = nullptr;
    {
        Core c;
        closed = c.core;
    }
    CHECK(tickHere(closed) == CLOCKWORK_TICK_NONE);
}
