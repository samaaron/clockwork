// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_pool.cpp — the guest's pool (clockwork_pool.h).
 *
 * The pool itself is Rust (rust/clockwork-heap/src/lib.rs): a segregated-fit
 * pool over a chain of areas the host hands it. This file runs it over the
 * guest's bulk span and adds what the engine needs around it: engine-thread
 * ownership with no locks, frees from other threads parked and drained, the
 * one spare the pool may grow into, the hooks the Rust router serves a Rust
 * guest's allocations through, and the RT router's block. An allocator is
 * exactly the kind of pointer arithmetic worth confining to one audited
 * file, and it corrupted the system heap once while it was not.
 *
 * Thread safety: the pool is owned by the engine thread and taken lock-free.
 * The engine thread (registered per block, and at boot) operates on it
 * directly; a foreign thread never touches it — its allocations come from the
 * system allocator, and its frees of pool memory are parked on a lock-free
 * Treiber stack (the freed block's own bytes carry the link) that the engine
 * drains on its next allocation or free. The law this serves: no locks of any
 * kind on the audio thread, and no malloc there beyond our arena management.
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
#include <array>
#include <atomic>
#include <cstdlib>
#include <thread>

// The spare's size: the one growth the pool may take without a malloc on the
// audio thread. 0 means no growth (the Teensy profile).
static constexpr size_t SPARE_BYTES = static_cast<size_t>(CLOCKWORK_GUEST_SPARE_BYTES);

extern "C" {
void*  clockwork_heap_pool_new(void* (*new_area)(void*, size_t), void (*free_area)(void*, void*),
                               void* area_ctx, size_t initial_size, size_t growth_size);
void   clockwork_heap_pool_free(void* pool);
void*  clockwork_heap_pool_alloc(void* pool, size_t bytes);
void   clockwork_heap_pool_dealloc(void* pool, void* ptr);
size_t clockwork_heap_pool_area_overhead(void);
size_t clockwork_heap_pool_used_bytes(const void* pool);
size_t clockwork_heap_pool_peak_bytes(const void* pool);
int    clockwork_router_arm(void* base, size_t bytes);   // rust/clockwork-heap/src/router.rs
void   clockwork_router_guest_hooks(void* (*take)(size_t), void (*give)(void*), int (*holds)(const void*));
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

static void*  g_pool = nullptr;           // the Rust pool's handle; NULL = no pool
static void*  g_span = nullptr;           // the guest's bulk span the pool runs over
static size_t g_span_bytes = 0;
static void*  g_pending_area = nullptr;   // the span, handed to the Rust pool's first area request
static void*  g_grown_into = nullptr;     // the spare the pool adopted, to give back at teardown
static size_t g_growths = 0;

// Address ranges of the pool's areas, so a free routes without asking the
// pool: the span, and the spare it grew into. Written on the engine thread
// (boot, or a growth), read lock-free from anywhere.
static constexpr int kMaxAreas = 2;
static std::array<std::atomic<uintptr_t>, kMaxAreas> g_area_base{};
static std::array<std::atomic<uintptr_t>, kMaxAreas> g_area_end{};
static std::atomic<int> g_area_count{0};

static void record_area(void* base, size_t size) {
    int i = g_area_count.load(std::memory_order_relaxed);
    if (i >= kMaxAreas) return;
    g_area_base[i].store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
    g_area_end[i].store(reinterpret_cast<uintptr_t>(base) + size, std::memory_order_relaxed);
    g_area_count.store(i + 1, std::memory_order_release);
}

bool clockwork_pool_holds(const void* ptr) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    const int n = g_area_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (a >= g_area_base[i].load(std::memory_order_relaxed) && a < g_area_end[i].load(std::memory_order_relaxed))
            return true;
    return false;
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

// ── The spare ───────────────────────────────────────────────────────────────
//
// The one growth area the pool may take without calling malloc: armed at
// boot and re-armed from a foreign thread (loader/egress drains) after the
// pool takes it. A request larger than the spare fails cleanly.
static std::atomic<void*> g_spare{nullptr};
static std::atomic<bool>  g_spare_wanted{false};

static void arm_spare() {
    if (SPARE_BYTES == 0 || g_spare.load(std::memory_order_relaxed)) return;
    if (void* spare = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, SPARE_BYTES))
        g_spare.store(spare, std::memory_order_release);
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
// pool is full) with the spare, once, and never with malloc.
static void* pool_new_area(void*, size_t size) {
    if (g_pending_area) {
        void* area = g_pending_area;
        g_pending_area = nullptr;
        record_area(area, size);
        return area;
    }
    if (SPARE_BYTES == 0 || size > SPARE_BYTES || g_grown_into) {
        g_spare_wanted.store(true, std::memory_order_release);
        return nullptr;
    }
    void* spare = g_spare.exchange(nullptr, std::memory_order_acquire);
    g_spare_wanted.store(true, std::memory_order_release);   // ask for the next either way
    if (!spare) return nullptr;
    g_grown_into = spare;
    record_area(spare, SPARE_BYTES);
    g_growths++;
    return spare;
}

static void pool_free_area(void*, void*) {
    // Areas are kept for the pool's life (frees route by the range table);
    // the span is the guest's and the spare goes back at teardown.
}

// The hooks the Rust router serves a Rust guest's allocations through.
static int   hook_holds(const void* p) { return clockwork_pool_holds(p) ? 1 : 0; }
static void* hook_take(size_t n)       { return clockwork_pool_alloc(n); }
static void  hook_give(void* p)        { clockwork_pool_free(p); }

// ── The API ─────────────────────────────────────────────────────────────────

static void forget_pool() {
    g_pool = nullptr;
    g_span = nullptr; g_span_bytes = 0; g_pending_area = nullptr;
    g_grown_into = nullptr; g_growths = 0;
    g_area_count.store(0, std::memory_order_relaxed);
    g_deferred_frees.store(nullptr, std::memory_order_relaxed);
}

void clockwork_pool_boot(void* bulk, size_t bytes) {
    clockwork_pool_register_engine_thread();
    router_arm_once();
    if (g_pool) {   // a rebuild: the old instance is gone, so the pool starts over on the same span
        clockwork_heap_pool_free(g_pool);
        if (g_grown_into) clockwork::mem::free(g_grown_into);
        forget_pool();
    }
    if (bulk && bytes > clockwork_heap_pool_area_overhead() + 64) {
        g_span = bulk; g_span_bytes = bytes;
        g_pending_area = bulk;
        g_pool = clockwork_heap_pool_new(pool_new_area, pool_free_area, nullptr, bytes, SPARE_BYTES);
        if (!g_pool) forget_pool();
    } else if (bulk) {
        clockwork_log("clockwork_pool_boot: the guest's bulk memory (%zu bytes) is too small for a pool", bytes);
    }
    clockwork_router_guest_hooks(g_pool ? hook_take : nullptr, g_pool ? hook_give : nullptr, g_pool ? hook_holds : nullptr);
    arm_spare();
}

void* clockwork_pool_alloc(size_t bytes) {
    if (!g_pool || !bytes) return nullptr;
    if (on_engine_thread()) {
        drain_on_engine_thread();
        return clockwork_heap_pool_alloc(g_pool, bytes);
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
    } else {
        stack_push(g_deferred_frees, ptr);
    }
}

void clockwork_pool_teardown() {
    clockwork_router_guest_hooks(nullptr, nullptr, nullptr);
    if (g_pool) clockwork_heap_pool_free(g_pool);
    clockwork_pool_foreign_maintenance();
    if (g_grown_into) clockwork::mem::free(g_grown_into);
    forget_pool();
    if (void* spare = g_spare.exchange(nullptr, std::memory_order_relaxed)) clockwork::mem::free(spare);
    g_spare_wanted.store(false, std::memory_order_relaxed);
}

void clockwork_pool_abandon() {
    clockwork_router_guest_hooks(nullptr, nullptr, nullptr);
    if (g_pool) clockwork_heap_pool_free(g_pool);   // the handle is a Box on the system heap, not in the arena
    forget_pool();
    g_system_frees.store(nullptr, std::memory_order_relaxed);
    g_spare.store(nullptr, std::memory_order_relaxed);
    g_spare_wanted.store(false, std::memory_order_relaxed);
}

size_t clockwork_pool_used_bytes() { return g_pool ? clockwork_heap_pool_used_bytes(g_pool) : 0; }
size_t clockwork_pool_peak_bytes() { return g_pool ? clockwork_heap_pool_peak_bytes(g_pool) : 0; }
size_t clockwork_pool_growths()    { return g_growths; }

uintptr_t clockwork_pool_spare_begin() { return (uintptr_t)g_spare.load(std::memory_order_acquire); }
size_t    clockwork_pool_spare_bytes() { return g_spare.load(std::memory_order_acquire) ? SPARE_BYTES : 0; }

void clockwork_pool_register_engine_thread() {
    const uint32_t gen = g_engine_reg_gen.load(std::memory_order_relaxed);
    if (gen != 0 && t_engine_reg_seen == gen)
        return;   // this thread registered last; nothing has changed since
    g_engine_thread.store(this_thread_key(), std::memory_order_relaxed);
    t_engine_reg_seen = g_engine_reg_gen.fetch_add(1, std::memory_order_relaxed) + 1;
}

void clockwork_pool_foreign_maintenance() {
    // Called from loader/egress threads: release engine-parked system
    // pointers and re-arm the spare if the pool asked for one.
    void* node = g_system_frees.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        void* next = *static_cast<void**>(node);
        clockwork::mem::free(node);
        node = next;
    }
    if (g_spare_wanted.exchange(false, std::memory_order_acquire) && SPARE_BYTES &&
        !g_spare.load(std::memory_order_relaxed)) {
        void* spare = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, SPARE_BYTES);
        if (spare) {
            void* expected = nullptr;
            if (!g_spare.compare_exchange_strong(expected, spare, std::memory_order_release, std::memory_order_relaxed))
                clockwork::mem::free(spare);   // lost the race; one spare is enough
        }
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
size_t clockwork_pool_growths()                        { return 0; }
uintptr_t clockwork_pool_spare_begin()                 { return 0; }
size_t    clockwork_pool_spare_bytes()                 { return 0; }
void   clockwork_pool_register_engine_thread()         {}
void   clockwork_pool_foreign_maintenance()            {}
uintptr_t clockwork_router_begin()                     { return 0; }
size_t    clockwork_router_bytes()                     { return 0; }
void      clockwork_router_reserve()                   {}
bool      clockwork_router_reclaim()                   { return true; }

#endif // CLOCKWORK_SYNTH
