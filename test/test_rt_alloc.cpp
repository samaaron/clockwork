// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_rt_alloc.cpp — the audio thread must not allocate.
 *
 * Defines the global operator new/delete overrides that read the thread-local
 * flag in src/rt_alloc.h. Clockwork sets that flag inside its block driver
 * (always on in production: two flag stores per callback, immeasurable against
 * the audio budget) and nothing reads it outside this binary. Allocations made
 * while it is set bump an atomic counter, which must stay at zero.
 *
 * This pins the Clockwork's discipline, not a DSP's. What the thing behind
 * dsp_process() does with memory is its own affair and is proved in its own
 * repo; what is proved here is that driving a block through clockwork —
 * draining the IN ring, classifying, parking and firing the schedule, calling
 * dsp_process, writing replies to the egress ring — allocates nothing.
 *
 * Caveat worth knowing: these hooks intercept C++ operator new/delete only.
 * A bare malloc from C code is invisible to them.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "rt_alloc.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <new>

// Under ThreadSanitizer the whole file reduces to one explicitly-skipped case:
// clang links TSan's C++ runtime statically and it defines the replaceable
// global allocation functions itself, so a second strong definition is a link
// error. A skip keeps the suite reporting the coverage gap rather than passing
// vacuously against hooks that never counted.
#if defined(RT_ALLOC_HOOKS_UNAVAILABLE)

TEST_CASE("rt-alloc: unsupported under ThreadSanitizer", "[rt_alloc]") {
    SKIP("global operator new/delete replacement conflicts with TSan's "
         "statically linked runtime");
}

#else

namespace {

void* rt_new(std::size_t n) {
    if (rt_alloc::g_in_rt) rt_alloc::g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void rt_delete(void* p) noexcept {
    if (p && rt_alloc::g_in_rt) rt_alloc::g_frees.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}

} // namespace

void* operator new(std::size_t n)                                   { return rt_new(n); }
void* operator new[](std::size_t n)                                 { return rt_new(n); }
void* operator new(std::size_t n,   const std::nothrow_t&) noexcept { try { return rt_new(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return rt_new(n); } catch (...) { return nullptr; } }
void  operator delete(void* p)                noexcept              { rt_delete(p); }
void  operator delete[](void* p)              noexcept              { rt_delete(p); }
void  operator delete(void* p, std::size_t)   noexcept              { rt_delete(p); }
void  operator delete[](void* p, std::size_t) noexcept              { rt_delete(p); }
void  operator delete(void* p,   const std::nothrow_t&) noexcept    { rt_delete(p); }
void  operator delete[](void* p, const std::nothrow_t&) noexcept    { rt_delete(p); }

TEST_CASE("rt-alloc: a steady-state block allocates nothing", "[rt_alloc]") {
    lanes_test::boot();
    lanes_test::tick();          // let anything one-time settle
    lanes_test::drainRt();

    rt_alloc::reset();
    {
        rt_alloc::Guard guard;   // covers the whole tick, not just the DSP pass
        for (int i = 0; i < 16; ++i) lanes_test::tick();
    }
    CHECK(rt_alloc::g_allocs.load(std::memory_order_relaxed) == 0);
    CHECK(rt_alloc::g_frees.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("rt-alloc: draining and answering OSC allocates nothing", "[rt_alloc]") {
    lanes_test::boot();

    // Queue a batch before arming the guard, so the writes themselves (which
    // happen on a control thread in a real host) are not counted.
    // "/clockwork/ping" is answered by clockwork; "/dummy/ping" is forwarded
    // across the boundary to dsp_osc and answered by the DSP through
    // DspHost::emit_osc. Both replies are written from inside the tick, so
    // this covers the callback direction as well as the call direction —
    // which matters, because DspHost::emit_osc is documented callable from
    // the audio thread and would be a lie if clockwork allocated to serve
    // it.
    for (int i = 0; i < 8; ++i) {
        const auto a = osc_test::message("/clockwork/ping", i);
        const auto b = osc_test::message("/dummy/ping");
        lanes_test::ingress(a.ptr(), a.size(), 1);
        lanes_test::ingress(b.ptr(), b.size(), 2);
    }

    rt_alloc::reset();
    {
        rt_alloc::Guard guard;
        for (int i = 0; i < 4; ++i) lanes_test::tick();
    }
    const int64_t allocs = rt_alloc::g_allocs.load(std::memory_order_relaxed);
    const int64_t frees  = rt_alloc::g_frees.load(std::memory_order_relaxed);
    rt_alloc::reset();
    lanes_test::drainRt();

    CHECK(allocs == 0);
    CHECK(frees == 0);
}

#endif // RT_ALLOC_HOOKS_UNAVAILABLE
