// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_pool.cpp — the guest's pool (clockwork_pool.h).
 *
 * The pool itself is Rust (rust/clockwork-heap/src/lib.rs): a segregated-fit
 * pool over a chain of areas the host hands it. This file runs it over the
 * guest's bulk span and adds what the engine needs around it: engine-thread
 * ownership with no locks, frees from other threads parked and drained, the
 * hooks the Rust router serves a Rust guest's allocations through, and the
 * RT router's block. The pool never grows (clockwork_pool.h). All of the
 * allocator's pointer arithmetic is confined to this one file.
 *
 * Thread safety: the pool is owned by the engine thread and taken lock-free.
 * The engine thread (registered per block, and at boot) operates on it
 * directly; a foreign thread never touches it — its allocations come from the
 * system allocator, and its frees of pool memory are parked on a lock-free
 * Treiber stack (the freed block's own bytes carry the link) that the engine
 * drains on its next allocation or free. This keeps locks of any kind off the
 * audio thread, and malloc there limited to our arena management.
 */

#include "clockwork_pool.h"
#include "memory_profile.h"
#include "clockwork_config.h"   // clockwork_log

// The body is compiled only where the audio path is (CLOCKWORK_SYNTH — see
// CMakeLists.txt, which is explicit that the flag gates clockwork's engine
// lifecycle and render, not the DSP). Without the path there is no guest to
// lend to, so the public API degrades to inert stubs.
#if CLOCKWORK_SYNTH

#include "mem_region.h"
#include "platform.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>

extern "C" {
void*  clockwork_heap_pool_new(void* (*new_area)(void*, size_t), void (*free_area)(void*, void*),
                               void* area_ctx, size_t initial_size, size_t growth_size);
void   clockwork_heap_pool_free(void* pool);
void*  clockwork_heap_pool_alloc(void* pool, size_t bytes);
void   clockwork_heap_pool_dealloc(void* pool, void* ptr);
size_t clockwork_heap_pool_area_overhead(void);
size_t clockwork_heap_pool_used_bytes(const void* pool);
size_t clockwork_heap_pool_peak_bytes(const void* pool);
uint32_t clockwork_heap_pool_double_frees(const void* pool);
int32_t  clockwork_heap_pool_check(const void* pool);
size_t   clockwork_heap_pool_check_bad(void);
size_t   clockwork_heap_pool_check_prev_payload(void);
size_t   clockwork_heap_pool_check_prev_size(void);
int    clockwork_router_arm(void* base, size_t bytes);   // rust/clockwork-heap/src/router.rs
void   clockwork_router_guest_hooks(void* (*take)(size_t), void (*give)(void*), int (*holds)(const void*));
}

static void* g_pool = nullptr;           // the Rust pool's handle; NULL = no pool

// CLOCKWORK_POOL_CHECK=1 in the environment: walk the whole pool after every
// allocation and free on the engine thread and stop at the first one that
// leaves it broken, naming the operation. A diagnostic, not a build setting:
// the walk is the whole pool, every time.
static int g_pool_check = -1;
static bool pool_check_wanted() {
    if (g_pool_check < 0) { const char* v = std::getenv("CLOCKWORK_POOL_CHECK"); g_pool_check = (v && *v && *v != '0') ? 1 : 0; }
    return g_pool_check == 1;
}
static void pool_check(const char* op, const void* ptr, size_t bytes) {
    if (!pool_check_wanted() || !g_pool) return;
    const int32_t code = clockwork_heap_pool_check(g_pool);
    if (code != 0) {
        const size_t bad = clockwork_heap_pool_check_bad(), prev = clockwork_heap_pool_check_prev_payload(), psz = clockwork_heap_pool_check_prev_size();
        clockwork_log("clockwork_pool: BROKEN after %s(0x%08x, %zu): check %d at block 0x%08x; the block before it is payload 0x%08x + %zu",
                      op, (unsigned)(uintptr_t)ptr, bytes, (int)code, (unsigned)bad, (unsigned)prev, psz);
        fprintf(stderr, "clockwork_pool: BROKEN after %s(%p, %zu): check %d at block %p; the block before it is payload %p + %zu\n",   // stdio-ok: a hunt
                op, ptr, bytes, (int)code, (void*)bad, (void*)prev, psz);
        std::abort();
    }
}

// ── Engine-thread ownership, no locks ───────────────────────────────────────
//
// The engine thread registers itself at boot and at every block start
// (audio_processor calls clockwork_pool_register_engine_thread()). Pool
// operations from that thread go straight in; pool frees from any other
// thread are parked on a lock-free stack the engine drains. Foreign-thread
// *allocations* never come from the pool at all — they use the system
// allocator on their own thread, and frees route by address range.

static std::atomic<size_t> g_engine_thread{0};   // hash of the owner's id

static inline size_t this_thread_key() {
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

// Registration generation: bumped by every registration and remembered by
// the thread that made it, so the thread that registered last can skip the
// lookup. `this_thread_key` is not free: on ESP-IDF `std::this_thread::get_id`
// is `pthread_self`, which walks the pthread list under a lock — 1,790 cycles
// of a 320,000-cycle block, measured, spent re-registering the same audio
// thread every block. Two threads taking turns (a device switch in flight)
// each see the other's generation and register as they did before, so this
// is never slower than the unconditional store it replaces.
static std::atomic<uint32_t> g_engine_reg_gen{0};
static thread_local uint32_t t_engine_reg_seen = 0;

static inline bool on_engine_thread() {
    size_t owner = g_engine_thread.load(std::memory_order_relaxed);
    return owner == 0 || owner == this_thread_key();
}

// A Treiber stack of pointers, the link stored in the pointee itself (every
// pool payload and every system block here is >= 16 aligned bytes).
static void stack_push(std::atomic<void*>& head_slot, void* ptr) {
    void* head = head_slot.load(std::memory_order_relaxed);
    for (;;) {
        *static_cast<void**>(ptr) = head;
        if (head_slot.compare_exchange_weak(head, ptr, std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

// ── The pool ────────────────────────────────────────────────────────────────

static void*  g_span = nullptr;           // the guest's bulk span the pool runs over
static size_t g_span_bytes = 0;
static void*  g_pending_area = nullptr;   // the span, handed to the Rust pool's first area request
// A guest's double free, refused by the pool: said once per occurrence, with
// the pointer, since the pool's own count is the only trace of it.
static uint32_t g_double_frees_seen = 0;

// The span's address range, so a free routes without asking the pool.
// Written on the engine thread at boot, read lock-free from anywhere.
static std::atomic<uintptr_t> g_area_base{0};
static std::atomic<uintptr_t> g_area_end{0};

bool clockwork_pool_holds(const void* ptr) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = g_area_end.load(std::memory_order_acquire);
    return end != 0 && a >= g_area_base.load(std::memory_order_relaxed) && a < end;
}

// Deferred frees from foreign threads, and system-owned pointers the engine
// thread wanted freed (released by any foreign-thread maintenance call).
static std::atomic<void*> g_deferred_frees{nullptr};
static std::atomic<void*> g_system_frees{nullptr};

static void drain_on_engine_thread() {
    void* node = g_deferred_frees.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        void* next = *static_cast<void**>(node);
        clockwork_heap_pool_dealloc(g_pool, node);
        node = next;
    }
}

// ── The RT router's block ───────────────────────────────────────────────────

static void*  g_router_block = nullptr;   // the RT router's, for the life of the process
static size_t g_router_bytes = 0;

// Arm the RT router, once: its block is the Rust subsystems' for the life of
// the process, so a second boot finds it armed and does nothing.
static void router_arm_once() {
    if (g_router_block || CLOCKWORK_ROUTER_BYTES < 65536) return;
    void* block = clockwork::mem::alloc(clockwork::mem::Tier::Fast, CLOCKWORK_ROUTER_BYTES);
    if (!block) { clockwork_log("clockwork_pool_boot: no %u bytes for the RT router; the Rust subsystems stay with the system allocator", (unsigned)CLOCKWORK_ROUTER_BYTES); return; }
    if (!clockwork_router_arm(block, CLOCKWORK_ROUTER_BYTES)) { clockwork::mem::free(block); return; }
    g_router_block = block;
    g_router_bytes = CLOCKWORK_ROUTER_BYTES;
}

void clockwork_router_reserve() { router_arm_once(); }

bool clockwork_router_reclaim() {
    if (!g_router_block) return true;   // never armed: nothing to find again
    void* again = clockwork::mem::alloc(clockwork::mem::Tier::Fast, g_router_bytes);
    return again == g_router_block;
}

uintptr_t clockwork_router_begin() { return (uintptr_t)g_router_block; }
size_t    clockwork_router_bytes() { return g_router_bytes; }

// ── Area callbacks handed to the Rust pool ──────────────────────────────────

// The first request is answered with the guest's span; a later one (the
// pool is full) with nothing: the pool never grows, and never mallocs.
static void* pool_new_area(void*, size_t size) {
    if (!g_pending_area) return nullptr;
    void* area = g_pending_area;
    g_pending_area = nullptr;
    g_area_base.store(reinterpret_cast<uintptr_t>(area), std::memory_order_relaxed);
    g_area_end.store(reinterpret_cast<uintptr_t>(area) + size, std::memory_order_release);
    return area;
}

static void pool_free_area(void*, void*) {
    // The span is the guest's: nothing to give back.
}

// The hooks the Rust router serves a Rust guest's allocations through.
static int   hook_holds(const void* p) { return clockwork_pool_holds(p) ? 1 : 0; }
static void* hook_take(size_t n)       { return clockwork_pool_alloc(n); }
static void  hook_give(void* p)        { clockwork_pool_free(p); }

// ── The API ─────────────────────────────────────────────────────────────────

static void forget_pool() {
    g_pool = nullptr;
    g_double_frees_seen = 0;
    g_span = nullptr; g_span_bytes = 0; g_pending_area = nullptr;
    g_area_end.store(0, std::memory_order_relaxed);
    g_area_base.store(0, std::memory_order_relaxed);
    g_deferred_frees.store(nullptr, std::memory_order_relaxed);
}

void clockwork_pool_boot(void* bulk, size_t bytes) {
    clockwork_pool_register_engine_thread();
    router_arm_once();
    if (g_pool) {   // a rebuild: the old instance is gone, so the pool starts over on the same span
        clockwork_heap_pool_free(g_pool);
        forget_pool();
    }
    if (bulk && bytes > clockwork_heap_pool_area_overhead() + 64) {
        g_span = bulk; g_span_bytes = bytes;
        g_pending_area = bulk;
        g_pool = clockwork_heap_pool_new(pool_new_area, pool_free_area, nullptr, bytes, /*growth*/ 0);
        if (!g_pool) forget_pool();
    } else if (bulk) {
        clockwork_log("clockwork_pool_boot: the guest's bulk memory (%zu bytes) is too small for a pool", bytes);
    }
    clockwork_router_guest_hooks(g_pool ? hook_take : nullptr, g_pool ? hook_give : nullptr, g_pool ? hook_holds : nullptr);
}

void* clockwork_pool_alloc(size_t bytes) {
    if (!g_pool || !bytes) return nullptr;
    if (on_engine_thread()) {
        drain_on_engine_thread();
        void* p = clockwork_heap_pool_alloc(g_pool, bytes);
        pool_check("alloc", p, bytes);
        return p;
    }
    // Foreign thread: system memory on its own thread, routed back to the
    // right place on free by the range check.
    return clockwork::mem::alloc(clockwork::mem::Tier::Bulk, bytes);
}

void clockwork_pool_free(void* ptr) {
    if (!ptr) return;
    if (!clockwork_pool_holds(ptr)) {
        // System memory (a foreign thread's allocation). Freeing it on the
        // engine thread would be a foreign malloc-family call there — park
        // it for a foreign-thread drain.
        if (g_pool && on_engine_thread()) stack_push(g_system_frees, ptr);
        else                              clockwork::mem::free(ptr);
        return;
    }
    if (on_engine_thread()) {
        drain_on_engine_thread();
        clockwork_heap_pool_dealloc(g_pool, ptr);
        pool_check("free", ptr, 0);
        const uint32_t d = clockwork_heap_pool_double_frees(g_pool);
        if (d != g_double_frees_seen) {
            g_double_frees_seen = d;
            clockwork_log("clockwork_pool_free: the guest freed 0x%08x twice (refused; %u so far)", (unsigned)(uintptr_t)ptr, (unsigned)d);
        }
    } else {
        stack_push(g_deferred_frees, ptr);
    }
}

void clockwork_pool_teardown() {
    clockwork_router_guest_hooks(nullptr, nullptr, nullptr);
    if (g_pool) clockwork_heap_pool_free(g_pool);
    clockwork_pool_foreign_maintenance();
    forget_pool();
}

void clockwork_pool_abandon() {
    clockwork_router_guest_hooks(nullptr, nullptr, nullptr);
    if (g_pool) clockwork_heap_pool_free(g_pool);   // the handle is a Box on the system heap, not in the arena
    forget_pool();
    g_system_frees.store(nullptr, std::memory_order_relaxed);
}

size_t clockwork_pool_used_bytes() { return g_pool ? clockwork_heap_pool_used_bytes(g_pool) : 0; }
size_t clockwork_pool_peak_bytes() { return g_pool ? clockwork_heap_pool_peak_bytes(g_pool) : 0; }

void clockwork_pool_register_engine_thread() {
    const uint32_t gen = g_engine_reg_gen.load(std::memory_order_relaxed);
    if (gen != 0 && t_engine_reg_seen == gen)
        return;   // this thread registered last; nothing has changed since
    g_engine_thread.store(this_thread_key(), std::memory_order_relaxed);
    t_engine_reg_seen = g_engine_reg_gen.fetch_add(1, std::memory_order_relaxed) + 1;
}

void clockwork_pool_foreign_maintenance() {
    // Called from loader/egress threads: release engine-parked system
    // pointers.
    void* node = g_system_frees.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        void* next = *static_cast<void**>(node);
        clockwork::mem::free(node);
        node = next;
    }
}

#else // !CLOCKWORK_SYNTH — inert stubs: no pool is created at all

void   clockwork_pool_boot(void*, size_t)              {}
void*  clockwork_pool_alloc(size_t)                    { return nullptr; }
void   clockwork_pool_free(void*)                      {}
bool   clockwork_pool_holds(const void*)               { return false; }
void   clockwork_pool_teardown()                       {}
void   clockwork_pool_abandon()                        {}
size_t clockwork_pool_used_bytes()                     { return 0; }
size_t clockwork_pool_peak_bytes()                     { return 0; }
void   clockwork_pool_register_engine_thread()         {}
void   clockwork_pool_foreign_maintenance()            {}
uintptr_t clockwork_router_begin()                     { return 0; }
size_t    clockwork_router_bytes()                     { return 0; }
void      clockwork_router_reserve()                   {}
bool      clockwork_router_reclaim()                   { return true; }

#endif // CLOCKWORK_SYNTH
