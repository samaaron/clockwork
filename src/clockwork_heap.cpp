// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_heap.cpp — the native growable heap.
 *
 * Wraps the pool in rust/clockwork-heap (the clockwork_heap_pool_* entry points below) and
 * feeds it backing memory from the tiered placement allocator (mem_region.h):
 * the initial area from Tier::Fast, growth areas from Tier::Bulk. On desktop/WASM/NIF both tiers are plain std::malloc, so the pool
 * is one region; an embedded build maps Fast to
 * internal SRAM (the boot-time hot set) and grows into Bulk (PSRAM).
 *
 * All allocations via clockwork_heap_alloc/free use this pool instead of
 * system malloc, making them RT-safe once allocated.
 *
 * Thread safety: the pool is owned by the engine thread and taken lock-free.
 * The engine thread (registered per block, and at init) operates on the pool
 * directly; a foreign thread never touches it — its allocations come from the
 * system allocator, and its frees of pool memory are parked on a lock-free
 * Treiber stack (the freed block's own bytes carry the link) that the engine
 * drains on its next allocation or free. The law this serves: no locks of any
 * kind on the audio thread, and no malloc there beyond our arena management.
 * Contention is minimal — only buffer commands touch this, infrequently.
 */

#include "clockwork_heap.h"
#include "memory_profile.h"
#include "clockwork_config.h"   // clockwork_log

// The body is compiled only where the audio path is (CLOCKWORK_SYNTH — see
// CMakeLists.txt, which is explicit that the flag gates clockwork's engine
// lifecycle and render, not the DSP). Everything that drives this heap arrives
// with that path: audio_processor's bring-up claims it, and the guest reaches
// it through DspHost::free_bytes. Without the path there is nothing to feed,
// so the public API degrades to inert stubs and allocations return nullptr.
#if CLOCKWORK_SYNTH

#include "mem_region.h"
#include "platform.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <vector>

// Growth increment when the pool is exhausted. Default 16MB; sized per device
// via memory_profile.h (the ESP32-S3 profile shrinks it to 256KB).
static constexpr size_t HEAP_GROWTH_SIZE = CLOCKWORK_HEAP_GROWTH_SIZE;

// The pool itself is Rust (rust/clockwork-heap/src/lib.rs); this is the
// opaque handle it hands back. An allocator is exactly the kind of pointer
// arithmetic worth confining to one audited file, and it corrupted the system
// heap once while it was not.
extern "C" {
void*  clockwork_heap_pool_new(void* (*new_area)(size_t), void (*free_area)(void*),
                        size_t initial_size, size_t growth_size);
void   clockwork_heap_pool_free(void* pool);
void*  clockwork_heap_pool_alloc(void* pool, size_t bytes);
void   clockwork_heap_pool_dealloc(void* pool, void* ptr);
void   clockwork_heap_pool_free_all(void* pool);
size_t clockwork_heap_pool_area_overhead(void);
}

static void* g_heap_pool = nullptr;
static void* g_heap_backing = nullptr;
static bool  g_prereserved = false;   // region owned by the host layout — never freed here

// ── Engine-thread ownership, no locks ───────────────────────────────────────
//
// The engine thread registers itself at init and at every block start
// (audio_processor calls clockwork_heap_register_engine_thread()). Pool
// operations from that thread go straight in; pool frees from any other
// thread are parked on a lock-free stack the engine drains. Foreign-thread
// *allocations* never come from the pool at all — they use
// the system allocator on their own thread, and frees route by address range.

#include <thread>

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

// Deferred frees from foreign threads: Treiber stack, link stored in the
// freed block itself (every pool block payload is >= 16 aligned bytes).
static std::atomic<void*> g_deferred_frees{nullptr};

static void deferred_push(void* ptr) {
    void* head = g_deferred_frees.load(std::memory_order_relaxed);
    for (;;) {
        *static_cast<void**>(ptr) = head;
        if (g_deferred_frees.compare_exchange_weak(head, ptr,
                std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

static void deferred_drain_on_engine_thread() {
    void* node = g_deferred_frees.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        void* next = *static_cast<void**>(node);
        clockwork_heap_pool_dealloc(g_heap_pool, node);
        node = next;
    }
}

// Address ranges of every pool area, so a free routes without asking the
// pool. Append-only; written by the thread that created the area (engine or
// boot), read lock-free from anywhere.
static constexpr int kMaxAreas = 32;
static std::atomic<uintptr_t> g_area_base[kMaxAreas];
static std::atomic<uintptr_t> g_area_end[kMaxAreas];
static std::atomic<int> g_area_count{0};

static void record_area(void* base, size_t size) {
    int i = g_area_count.load(std::memory_order_relaxed);
    if (i >= kMaxAreas) return;   // untracked area: frees of it will misroute
                                  // to the system allocator — count kept low
                                  // by the growth policy below.
    g_area_base[i].store(reinterpret_cast<uintptr_t>(base), std::memory_order_relaxed);
    g_area_end[i].store(reinterpret_cast<uintptr_t>(base) + size, std::memory_order_relaxed);
    g_area_count.store(i + 1, std::memory_order_release);
}

static bool in_pool(void* ptr) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    const int n = g_area_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (a >= g_area_base[i].load(std::memory_order_relaxed) &&
            a <  g_area_end[i].load(std::memory_order_relaxed))
            return true;
    return false;
}

// The one growth area the engine may take without calling malloc: pre-armed
// at init and re-armed from a foreign thread (loader/egress drains) after the
// engine takes it. A request larger than the spare fails cleanly.
static std::atomic<void*> g_spare_area{nullptr};
static std::atomic<bool> g_spare_wanted{false};

static size_t g_initial_size = 0;
static size_t g_total_allocated = 0;
static size_t g_growth_count = 0;

// Area callbacks handed to the Rust pool (its new_area / free_area)
// First call returns the pre-allocated backing block. Subsequent calls (when
// areaMoreSize > 0 and the pool is exhausted) malloc new areas on demand.

static void* g_pending_area = nullptr;

static void* heap_new_area(size_t size) {
    if (g_pending_area) {
        // Initial allocation — return the pre-allocated backing block
        void* ptr = g_pending_area;
        g_pending_area = nullptr;
        record_area(ptr, size);
        return ptr;
    }

    // Growth: only the pre-armed spare — the engine thread never mallocs.
    // (HEAP_GROWTH_SIZE is the spare's size; a larger request fails clean.)
    if (size > HEAP_GROWTH_SIZE)
        return nullptr;
    void* spare = g_spare_area.exchange(nullptr, std::memory_order_acquire);
    if (!spare) {
        g_spare_wanted.store(true, std::memory_order_release);
        return nullptr;
    }
    g_spare_wanted.store(true, std::memory_order_release);  // ask for the next
    record_area(spare, HEAP_GROWTH_SIZE);
    g_total_allocated += HEAP_GROWTH_SIZE;
    g_growth_count++;
    return spare;
}

static void heap_free_area(void*) {
    // Areas are kept for the pool's life (the range table is append-only and
    // frees route by it); everything is returned in clockwork_heap_destroy.
}

void clockwork_heap_init(size_t bytes) {
    if (g_heap_pool) {
        // Pool exists — reset it (all old allocations are abandoned since
        // a fresh dsp_new is about to run). No need to reallocate.
        clockwork_heap_pool_free_all(g_heap_pool);
        return;
    }

    // Bound the Fast initial area by the largest free Fast block (minus a reserve
    // for Fast allocations made later) so we never request more contiguous internal
    // SRAM than exists. A larger request would fall back to Bulk and put the whole
    // pool — including the per-sample audio buses and wire scratch — in slow RAM.
    // Overflow still grows into Bulk on demand via heap_new_area. On desktop/WASM/
    // NIF largest_free() is SIZE_MAX, so bytes is unchanged.
    {
        const size_t avail = clockwork::mem::largest_free(clockwork::mem::Tier::Fast);
        const size_t reserve = (size_t)CLOCKWORK_FAST_RESERVE + clockwork_heap_pool_area_overhead();
        const size_t cap = (avail > reserve) ? (avail - reserve) : 0;
        if (bytes > cap)
            bytes = cap;
    }

    // The pool asks its callback for the usable size plus its own bookkeeping,
    // so the backing block has to cover both.
    size_t total = bytes + clockwork_heap_pool_area_overhead();
    // Initial area — Fast tier (internal SRAM on embedded, malloc on desktop),
    // requested strictly so a Fast placement failure is logged before we fall back
    // to Bulk ourselves. On desktop both tiers are malloc.
    g_heap_backing = clockwork::mem::alloc(clockwork::mem::Tier::Fast, total, /*allow_spill=*/false);
    if (!g_heap_backing) {
        clockwork_log("clockwork_heap_init: %zu bytes did not fit Fast, falling back to Bulk", total);
        g_heap_backing = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, total);
    }
    if (!g_heap_backing) {
        clockwork_log("clockwork_heap_init: failed to allocate %zu bytes", total);
        return;
    }

    g_initial_size = total;
    g_total_allocated = total;
    g_growth_count = 0;

    // The pool's first area request is answered with this block.
    g_pending_area = g_heap_backing;

    // A non-zero growth size lets the pool add areas when it runs out —
    // served exclusively from the pre-armed spare (heap_new_area).
    g_heap_pool = clockwork_heap_pool_new(heap_new_area, heap_free_area, total, HEAP_GROWTH_SIZE);
    clockwork_heap_register_engine_thread();
    if (!g_spare_area.load(std::memory_order_relaxed)) {
        if (void* spare = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, HEAP_GROWTH_SIZE))
            g_spare_area.store(spare, std::memory_order_release);
    }
}

void* clockwork_heap_alloc(size_t bytes) {
    if (!g_heap_pool)
        return nullptr;
    if (on_engine_thread()) {
        deferred_drain_on_engine_thread();
        return clockwork_heap_pool_alloc(g_heap_pool, bytes);
    }
    // Foreign thread: system memory on its own thread,
    // routed back to the right allocator on free by the range check.
    return clockwork::mem::alloc(clockwork::mem::Tier::Bulk, bytes);
}

static void deferred_push_system(void* ptr);

void clockwork_heap_free(void* ptr) {
    if (!ptr)
        return;
    if (!in_pool(ptr)) {
        // System memory. Freeing it on the engine thread would be a foreign
        // malloc-family call there — park it for a foreign-thread drain.
        if (g_heap_pool && on_engine_thread())
            deferred_push_system(ptr);
        else
            clockwork::mem::free(ptr);
        return;
    }
    if (on_engine_thread()) {
        deferred_drain_on_engine_thread();
        clockwork_heap_pool_dealloc(g_heap_pool, ptr);
    } else {
        deferred_push(ptr);
    }
}

// System-owned pointers the engine thread wanted freed: parked here and
// released by any foreign-thread maintenance call.
static std::atomic<void*> g_system_frees{nullptr};

static void deferred_push_system(void* ptr) {
    void* head = g_system_frees.load(std::memory_order_relaxed);
    for (;;) {
        *static_cast<void**>(ptr) = head;
        if (g_system_frees.compare_exchange_weak(head, ptr,
                std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

void clockwork_heap_register_engine_thread() {
    const uint32_t gen = g_engine_reg_gen.load(std::memory_order_relaxed);
    if (gen != 0 && t_engine_reg_seen == gen)
        return;   // this thread registered last; nothing has changed since
    g_engine_thread.store(this_thread_key(), std::memory_order_relaxed);
    t_engine_reg_seen = g_engine_reg_gen.fetch_add(1, std::memory_order_relaxed) + 1;
}

void clockwork_heap_foreign_maintenance() {
    // Called from loader/egress threads: release engine-parked system
    // pointers and re-arm the growth spare if the engine asked for one.
    void* node = g_system_frees.exchange(nullptr, std::memory_order_acquire);
    while (node) {
        void* next = *static_cast<void**>(node);
        clockwork::mem::free(node);
        node = next;
    }
    if (g_spare_wanted.exchange(false, std::memory_order_acquire) &&
        !g_spare_area.load(std::memory_order_relaxed)) {
        void* spare = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, HEAP_GROWTH_SIZE);
        if (spare) {
            void* expected = nullptr;
            if (!g_spare_area.compare_exchange_strong(expected, spare,
                    std::memory_order_release, std::memory_order_relaxed))
                clockwork::mem::free(spare);   // lost the race; one spare is enough
        }
    }
}

void clockwork_heap_destroy() {
    if (g_heap_pool) {
        clockwork_heap_pool_free(g_heap_pool);
        g_heap_pool = nullptr;
    }
    if (g_prereserved) {
        // The region belongs to the host's memory layout; hand nothing back.
        g_deferred_frees.store(nullptr, std::memory_order_relaxed);
        g_area_count.store(0, std::memory_order_relaxed);
        g_spare_area.store(nullptr, std::memory_order_relaxed);
        g_heap_backing = nullptr;
        g_total_allocated = 0;
        g_growth_count = 0;
        g_prereserved = false;
        return;
    }
    // Free growth areas recorded in the range table (entry 0 is the initial
    // backing block, freed below), and anything still parked.
    clockwork_heap_foreign_maintenance();
    g_deferred_frees.store(nullptr, std::memory_order_relaxed);
    const int n = g_area_count.load(std::memory_order_relaxed);
    for (int i = 1; i < n; ++i)
        clockwork::mem::free(reinterpret_cast<void*>(g_area_base[i].load(std::memory_order_relaxed)));
    g_area_count.store(0, std::memory_order_relaxed);
    if (void* spare = g_spare_area.exchange(nullptr, std::memory_order_relaxed))
        clockwork::mem::free(spare);
    // Free initial backing block
    if (g_heap_backing) {
        clockwork::mem::free(g_heap_backing);
        g_heap_backing = nullptr;
    }
    g_total_allocated = 0;
    g_growth_count = 0;
}

uintptr_t clockwork_heap_backing_end() {
    return g_heap_backing ? ((uintptr_t)g_heap_backing + g_initial_size) : 0;
}

size_t clockwork_heap_total_allocated() {
    return g_total_allocated;
}

size_t clockwork_heap_growth_count() {
    return g_growth_count;
}

#else // !CLOCKWORK_SYNTH — inert stubs: no pool is created at all

void   clockwork_heap_init(size_t)        {}
void*  clockwork_heap_alloc(size_t)       { return nullptr; }
void   clockwork_heap_free(void*)         {}
void   clockwork_heap_register_engine_thread() {}
void   clockwork_heap_foreign_maintenance()    {}
void   clockwork_heap_destroy()           {}
size_t clockwork_heap_total_allocated()   { return 0; }
size_t clockwork_heap_growth_count()      { return 0; }

#endif // CLOCKWORK_SYNTH
