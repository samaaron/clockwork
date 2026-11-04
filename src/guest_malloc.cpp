// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//
// guest_malloc.cpp — see guest_malloc.h.
//
// The wrappers exist only where the link routes the program's calls through
// them (CLOCKWORK_COUNT_GUEST_MALLOC, set by CMake with -Wl,--wrap=...). A
// call made inside the allocator itself — calloc's own malloc, say — is a
// reference the linker never sees as undefined, so it is not wrapped and
// nothing is counted twice. The C++ library's operator new reaches malloc
// through an undefined reference, so a guest's `new` is counted like its
// malloc.

#include "guest_malloc.h"
#include "guest_fence.h"
#include "core_state.h"   // clockwork::GuestMallocs

#include <atomic>

namespace {
// Every call, any scope, and the net bytes they hold (guest_malloc.h): the
// process's.
std::atomic<uint32_t> g_all_calls{0};
std::atomic<int64_t>  g_all_held{0};
uint32_t g_mark_calls = 0;
int64_t  g_mark_held  = 0;
bool     g_host_scratch = false;   // engine thread only: inside clockwork_worklet_alloc/free

#if CLOCKWORK_COUNT_GUEST_MALLOC
// Only the wrappers below call these, and they exist only where the link
// wraps. A call is the guest's when this thread is inside a guest call, and
// it is charged to that call's core.
inline clockwork::GuestMallocs* guest_call(ClockworkEntryKind& kind) {
    const ClockworkEntry here = clockwork_core_entry();
    kind = here.kind;
    if (kind != CLOCKWORK_ENTRY_GUEST_BOOT && kind != CLOCKWORK_ENTRY_GUEST_PROCESSING) return nullptr;
    clockwork::CoreState* st = clockwork::state(here.core);
    return st ? &st->guest_mallocs : nullptr;
}
inline void count_call(clockwork::GuestMallocs& g, ClockworkEntryKind kind) {
    if (kind == CLOCKWORK_ENTRY_GUEST_BOOT) g.boot.fetch_add(1, std::memory_order_relaxed);
    else                                    g.processing.fetch_add(1, std::memory_order_relaxed);
}
#endif
}  // namespace

// A call within the one this thread is in: the same core and gate.
static ClockworkEntry guest_call_entry(GuestCall kind) {
    ClockworkEntry e = clockwork_core_entry();
    e.kind = kind == GuestCall::Boot ? CLOCKWORK_ENTRY_GUEST_BOOT : CLOCKWORK_ENTRY_GUEST_PROCESSING;
    return e;
}

GuestCallScope::GuestCallScope(GuestCall kind) : previous_(clockwork_core_enter(guest_call_entry(kind))) {
    clockwork_guest_fence_close();   // from here the arena is the guest's regions only (guest_fence.h)
}
GuestCallScope::~GuestCallScope() {
    clockwork_guest_fence_open();
    clockwork_core_leave(previous_);
}

size_t clockwork_guest_malloc_bytes(const clockwork::CoreState& st) {
    const int64_t held = st.guest_mallocs.held.load(std::memory_order_relaxed);
    return held > 0 ? (size_t)held : 0;
}
uint32_t clockwork_guest_mallocs_boot(const clockwork::CoreState& st)       { return st.guest_mallocs.boot.load(std::memory_order_relaxed); }
uint32_t clockwork_guest_mallocs_processing(const clockwork::CoreState& st) { return st.guest_mallocs.processing.load(std::memory_order_relaxed); }
uint32_t clockwork_guest_frees(const clockwork::CoreState& st)              { return st.guest_mallocs.frees.load(std::memory_order_relaxed); }

void clockwork_malloc_host_scratch_begin() { g_host_scratch = true; }
void clockwork_malloc_host_scratch_end()   { g_host_scratch = false; }

extern "C" void clockwork_malloc_mark_boot(void) {
    g_mark_calls = g_all_calls.load(std::memory_order_relaxed);
    g_mark_held  = g_all_held.load(std::memory_order_relaxed);
}
uint32_t clockwork_mallocs_after_boot() { return g_all_calls.load(std::memory_order_relaxed) - g_mark_calls; }
size_t clockwork_malloc_bytes_after_boot() {
    const int64_t d = g_all_held.load(std::memory_order_relaxed) - g_mark_held;
    return d > 0 ? (size_t)d : 0;
}

void clockwork_guest_malloc_reset(clockwork::CoreState& st) {
    // held stays: the bytes are still taken, whichever instance took them
    st.guest_mallocs.boot.store(0, std::memory_order_relaxed);
    st.guest_mallocs.processing.store(0, std::memory_order_relaxed);
    st.guest_mallocs.frees.store(0, std::memory_order_relaxed);
}

#if CLOCKWORK_COUNT_GUEST_MALLOC

#include <malloc.h>   // malloc_usable_size: emscripten's dlmalloc and glibc both answer

bool clockwork_guest_malloc_counted() { return true; }

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void  __real_free(void*);

void* __wrap_malloc(size_t n) {
    void* p = __real_malloc(n);
    if (!g_host_scratch) {
        g_all_calls.fetch_add(1, std::memory_order_relaxed);
        if (p) g_all_held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    ClockworkEntryKind kind;
    if (clockwork::GuestMallocs* g = guest_call(kind)) {
        count_call(*g, kind);
        if (p) g->held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    return p;
}

void* __wrap_calloc(size_t count, size_t size) {
    void* p = __real_calloc(count, size);
    if (!g_host_scratch) {
        g_all_calls.fetch_add(1, std::memory_order_relaxed);
        if (p) g_all_held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    ClockworkEntryKind kind;
    if (clockwork::GuestMallocs* g = guest_call(kind)) {
        count_call(*g, kind);
        if (p) g->held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    return p;
}

void* __wrap_realloc(void* p, size_t n) {
    ClockworkEntryKind kind;
    clockwork::GuestMallocs* const g = guest_call(kind);
    const int64_t before = p ? (int64_t)malloc_usable_size(p) : 0;
    void* q = __real_realloc(p, n);
    if (!g_host_scratch) {
        g_all_calls.fetch_add(1, std::memory_order_relaxed);
        if (q || n == 0) g_all_held.fetch_sub(before, std::memory_order_relaxed);
        if (q) g_all_held.fetch_add((int64_t)malloc_usable_size(q), std::memory_order_relaxed);
    }
    if (g) {
        count_call(*g, kind);
        // realloc frees p when it succeeds, and when asked for 0; on failure p is untouched
        if (q || n == 0) g->held.fetch_sub(before, std::memory_order_relaxed);
        if (q) g->held.fetch_add((int64_t)malloc_usable_size(q), std::memory_order_relaxed);
    }
    return q;
}

void __wrap_free(void* p) {
    if (p && !g_host_scratch) g_all_held.fetch_sub((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    ClockworkEntryKind kind;
    clockwork::GuestMallocs* const g = p ? guest_call(kind) : nullptr;
    if (g) {
        g->frees.fetch_add(1, std::memory_order_relaxed);
        g->held.fetch_sub((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    __real_free(p);
}
}  // extern "C"

#else

bool clockwork_guest_malloc_counted() { return false; }

#endif
