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

#include <atomic>

// The Rust router's guest scope (rust/clockwork-heap/src/router.rs): while a
// guest entry point is open, a Rust guest's allocations go to the guest's
// pool. Present wherever the audio path is, which is wherever a guest is.
#if CLOCKWORK_SYNTH
extern "C" void clockwork_router_guest_enter(void);
extern "C" void clockwork_router_guest_exit(void);
#else
static inline void clockwork_router_guest_enter() {}
static inline void clockwork_router_guest_exit() {}
#endif

namespace {
GuestCall g_open = GuestCall::None;                  // engine thread only
std::atomic<int64_t>  g_held{0};                      // bytes the guest holds from malloc; signed so a free of something taken outside a guest call cannot wrap
std::atomic<uint32_t> g_mallocs_boot{0};
std::atomic<uint32_t> g_mallocs_processing{0};
std::atomic<uint32_t> g_frees{0};
// Every call, any scope, and the net bytes they hold (guest_malloc.h).
std::atomic<uint32_t> g_all_calls{0};
std::atomic<int64_t>  g_all_held{0};
uint32_t g_mark_calls = 0;
int64_t  g_mark_held  = 0;
bool     g_host_scratch = false;   // engine thread only: inside clockwork_worklet_alloc/free

#if CLOCKWORK_COUNT_GUEST_MALLOC
// Only the wrappers below call this, and they exist only where the link wraps.
inline void count_call() {
    if (g_open == GuestCall::Boot)            g_mallocs_boot.fetch_add(1, std::memory_order_relaxed);
    else if (g_open == GuestCall::Processing) g_mallocs_processing.fetch_add(1, std::memory_order_relaxed);
}
#endif
}  // namespace

void clockwork_guest_call_begin(GuestCall kind) { g_open = kind; clockwork_router_guest_enter(); }
void clockwork_guest_call_end() { clockwork_router_guest_exit(); g_open = GuestCall::None; }

size_t clockwork_guest_malloc_bytes() {
    const int64_t held = g_held.load(std::memory_order_relaxed);
    return held > 0 ? (size_t)held : 0;
}
uint32_t clockwork_guest_mallocs_boot()       { return g_mallocs_boot.load(std::memory_order_relaxed); }
uint32_t clockwork_guest_mallocs_processing() { return g_mallocs_processing.load(std::memory_order_relaxed); }
uint32_t clockwork_guest_frees()              { return g_frees.load(std::memory_order_relaxed); }

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

void clockwork_guest_malloc_reset() {
    // g_held stays: the bytes are still taken, whichever instance took them
    g_mallocs_boot.store(0, std::memory_order_relaxed);
    g_mallocs_processing.store(0, std::memory_order_relaxed);
    g_frees.store(0, std::memory_order_relaxed);
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
    if (g_open != GuestCall::None) {
        count_call();
        if (p) g_held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    return p;
}

void* __wrap_calloc(size_t count, size_t size) {
    void* p = __real_calloc(count, size);
    if (!g_host_scratch) {
        g_all_calls.fetch_add(1, std::memory_order_relaxed);
        if (p) g_all_held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    if (g_open != GuestCall::None) {
        count_call();
        if (p) g_held.fetch_add((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    return p;
}

void* __wrap_realloc(void* p, size_t n) {
    const bool open = g_open != GuestCall::None;
    const int64_t before = p ? (int64_t)malloc_usable_size(p) : 0;
    void* q = __real_realloc(p, n);
    if (!g_host_scratch) {
        g_all_calls.fetch_add(1, std::memory_order_relaxed);
        if (q || n == 0) g_all_held.fetch_sub(before, std::memory_order_relaxed);
        if (q) g_all_held.fetch_add((int64_t)malloc_usable_size(q), std::memory_order_relaxed);
    }
    if (open) {
        count_call();
        // realloc frees p when it succeeds, and when asked for 0; on failure p is untouched
        if (q || n == 0) g_held.fetch_sub(before, std::memory_order_relaxed);
        if (q) g_held.fetch_add((int64_t)malloc_usable_size(q), std::memory_order_relaxed);
    }
    return q;
}

void __wrap_free(void* p) {
    if (p && !g_host_scratch) g_all_held.fetch_sub((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    if (p && g_open != GuestCall::None) {
        g_frees.fetch_add(1, std::memory_order_relaxed);
        g_held.fetch_sub((int64_t)malloc_usable_size(p), std::memory_order_relaxed);
    }
    __real_free(p);
}
}  // extern "C"

#else

bool clockwork_guest_malloc_counted() { return false; }

#endif
