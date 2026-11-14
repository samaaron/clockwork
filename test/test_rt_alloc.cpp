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
#include <cstring>
#include <new>
#include <string>

#include "clockwork_prefix.h"
#include "memory_profile.h"
#include "params/clockwork_params.h"

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

// The parameter verbs read a peer's frame and answer inside the block.
TEST_CASE("rt-alloc: the parameter verbs allocate nothing", "[rt_alloc][params]") {
    lanes_test::boot();
    ClockworkParamEntry e[2] = {};
    e[0].id = 7000; e[0].kind = CLOCKWORK_PARAM_CONTINUOUS; e[0].max = 1.f; std::strcpy(e[0].name, "a");
    e[1].id = 7001; e[1].kind = CLOCKWORK_PARAM_STEPPED; e[1].max = 7.f; std::strcpy(e[1].name, "b");
    const std::string doc = std::string("\x02") + "what it does" + std::string(1, '\0');
    e[0].text_bytes = static_cast<uint32_t>(doc.size());
    osc_test::Builder defines;
    defines.begin(CLOCKWORK_SYS("param/define"))
        << osc::Blob(e, static_cast<osc::osc_bundle_element_size_t>(sizeof e))
        << osc::Blob(doc.data(), static_cast<osc::osc_bundle_element_size_t>(doc.size()));
    const auto define = defines.end();
    const auto text = osc_test::message(CLOCKWORK_SYS("param/text"), int32_t{ 7000 });
    osc_test::Builder sets;
    sets.begin(CLOCKWORK_SYS("param/set")) << osc::int32(7000) << 0.5f << osc::int32(7001) << 2.f;
    const auto set = sets.end();
    const auto list = osc_test::message(CLOCKWORK_SYS("param/list"));
    const auto unknown = osc_test::message(CLOCKWORK_SYS("param/set"), int32_t{ 7999 });   // refused
    ClockworkParamEntry gone[2] = {};
    gone[0].id = 7000; gone[1].id = 7001;
    const auto retire = osc_test::messageWithBlob(CLOCKWORK_SYS("param/define"), gone, sizeof gone);
    for (const auto* p : { &define, &set, &list, &text, &unknown, &retire })
        lanes_test::ingress(p->ptr(), p->size(), 1);

    rt_alloc::reset();
    {
        rt_alloc::Guard guard;
        lanes_test::tick();
    }
    const int64_t allocs = rt_alloc::g_allocs.load(std::memory_order_relaxed);
    const int64_t frees  = rt_alloc::g_frees.load(std::memory_order_relaxed);
    rt_alloc::reset();
    const auto out = lanes_test::drainRt();
    CHECK(allocs == 0);
    CHECK(frees == 0);
    // It did the work it was measured doing.
    bool defined = false, listed = false, texted = false;
    for (const auto& f : out) {
        const auto addr = osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size()));
        defined = defined || addr == CLOCKWORK_SYS("param/define.reply");
        listed = listed || addr == CLOCKWORK_SYS("param/list.reply");
        texted = texted || addr == CLOCKWORK_SYS("param/text.reply");
    }
    CHECK(defined);
    CHECK(listed);
    CHECK(texted);
}

// Closing the gaps in the text happens inside a define, so inside a block.
TEST_CASE("rt-alloc: text that has to move allocates nothing", "[rt_alloc][params]") {
    lanes_test::boot();
    ClockworkParamEntry keep{}, churn{};
    keep.id = 7100; keep.kind = CLOCKWORK_PARAM_CONTINUOUS; keep.max = 1.f; std::strcpy(keep.name, "keep");
    churn.id = 7101; churn.kind = CLOCKWORK_PARAM_CONTINUOUS; churn.max = 1.f; std::strcpy(churn.name, "churn");
    const std::string run = std::string("\x02") + std::string(CLOCKWORK_PARAM_TEXT_RUN_BYTES - 2u, 'x') + std::string(1, '\0');
    const auto defineOf = [&](ClockworkParamEntry e) {
        e.text_bytes = static_cast<uint32_t>(run.size());
        osc_test::Builder b;
        b.begin(CLOCKWORK_SYS("param/define"))
            << osc::Blob(&e, static_cast<osc::osc_bundle_element_size_t>(sizeof e))
            << osc::Blob(run.data(), static_cast<osc::osc_bundle_element_size_t>(run.size()));
        return b.end();
    };
    const auto keeping = defineOf(keep);
    const auto churning = defineOf(churn);
    lanes_test::ingress(keeping.ptr(), keeping.size(), 1);
    lanes_test::tick();
    lanes_test::drainRt();
    int64_t allocs = 0, frees = 0;
    const uint32_t rounds = 2u * CLOCKWORK_PARAM_TEXT_BYTES / CLOCKWORK_PARAM_TEXT_RUN_BYTES;
    uint32_t landed = 0;
    for (uint32_t i = 0; i < rounds; ++i) {
        lanes_test::ingress(churning.ptr(), churning.size(), 1);
        rt_alloc::reset();
        {
            rt_alloc::Guard guard;
            lanes_test::tick();
        }
        allocs += rt_alloc::g_allocs.load(std::memory_order_relaxed);
        frees  += rt_alloc::g_frees.load(std::memory_order_relaxed);
        rt_alloc::reset();
        for (const auto& f : lanes_test::drainRt())
            if (osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size())) == CLOCKWORK_SYS("param/define.reply")) ++landed;
    }
    CHECK(allocs == 0);
    CHECK(frees == 0);
    CHECK(landed == rounds);   // past the region's size, so the text moved on the way
}

#endif // RT_ALLOC_HOOKS_UNAVAILABLE
