// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * mem_region.cpp — tiered memory placement backends. See mem_region.h.
 *
 * Two implementations selected at compile time:
 *   - ESP_PLATFORM: a real two-tier backend (internal SRAM / PSRAM) with a small
 *     per-allocation header so free() is tier-agnostic and usage is accounted.
 *   - everything else (desktop / WASM / NIF): std::malloc / std::free, until a
 *     host calls set_arena() — then a first-fit arena over the span it gave,
 *     which is how wasm gets bounded allocation without emscripten's malloc
 *     walking into the guest's region. See set_arena() in mem_region.h.
 *
 * Compiled for every target, wasm included.
 */
#include "mem_region.h"
#include "platform.h"

#include <cstdlib>
#include <cstdint>

#if CLOCKWORK_HAS_TIERED_MEMORY

#include <atomic>
#include "esp_heap_caps.h"

namespace clockwork::mem {
namespace {

// Per-tier bytes currently handed out (requested sizes). Atomic because areas
// are (re)allocated from control/loader threads, never the audio thread.
std::atomic<size_t> g_in_use[2] = {{0}, {0}};

// Fast requests that fell back to Bulk (Fast exhausted); read via spill_count()
// so a host can report that data intended for internal SRAM landed in PSRAM.
std::atomic<size_t> g_spill_count{0};

constexpr uint32_t kCapsFast = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kCapsBulk = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

// Every allocation carries a 16-byte header so free() is tier-agnostic and
// accounting stays exact. 16 bytes == one alignment slot, so given a 16-aligned
// base the user pointer (base + 16) is also 16-aligned (SC_MEMORY_ALIGNMENT).
struct alignas(16) Header {
    uint32_t tier;   // Tier value (0 Fast, 1 Bulk)
    uint32_t magic;  // guards free() against foreign pointers
    size_t   bytes;  // requested size, for in_use() accounting
};
static_assert(sizeof(Header) == 16, "Header must occupy exactly one 16-byte slot");
constexpr uint32_t kMagic = 0x4D454D52; // 'MEMR'

// Allocate `total` bytes, >= 16-byte aligned, from the physical memory that backs
// `tier`. With allow_spill, an unsatisfiable request falls back to the other
// region (Fast->Bulk fall-backs are counted); without it, returns nullptr so the
// caller can react to the placement failure itself.
void* backend_alloc(Tier tier, size_t total, bool allow_spill) {
    const uint32_t first  = (tier == Tier::Fast) ? kCapsFast : kCapsBulk;
    void* p = heap_caps_aligned_alloc(16, total, first);
    if (p)
        return p;
    if (!allow_spill)
        return nullptr;
    const uint32_t second = (tier == Tier::Fast) ? kCapsBulk : kCapsFast;
    p = heap_caps_aligned_alloc(16, total, second); // graceful spill
    if (p && tier == Tier::Fast)
        g_spill_count.fetch_add(1, std::memory_order_relaxed);
    return p;
}

} // namespace

void* alloc(Tier tier, size_t bytes, bool allow_spill) {
    if (bytes == 0)
        return nullptr;
    void* base = backend_alloc(tier, bytes + sizeof(Header), allow_spill);
    if (!base)
        return nullptr;
    auto* h = static_cast<Header*>(base);
    h->tier = static_cast<uint32_t>(tier);
    h->magic = kMagic;
    h->bytes = bytes;
    g_in_use[h->tier & 1].fetch_add(bytes, std::memory_order_relaxed);
    return static_cast<char*>(base) + sizeof(Header);
}

void free(void* ptr) {
    if (!ptr)
        return;
    auto* h = reinterpret_cast<Header*>(static_cast<char*>(ptr) - sizeof(Header));
    if (h->magic == kMagic) {
        g_in_use[h->tier & 1].fetch_sub(h->bytes, std::memory_order_relaxed);
        heap_caps_free(h);
    } else {
        heap_caps_free(ptr); // not ours (shouldn't happen) — release without underflow
    }
}

size_t in_use(Tier tier) {
    return g_in_use[static_cast<int>(tier) & 1].load(std::memory_order_relaxed);
}

size_t largest_free(Tier tier) {
    return heap_caps_get_largest_free_block(tier == Tier::Fast ? kCapsFast : kCapsBulk);
}

size_t spill_count() {
    return g_spill_count.load(std::memory_order_relaxed);
}

// A tiered device places into real regions by capability; there is nothing a
// single host-owned span could improve, so the arena entry points do nothing
// and report nothing. Callers stay identical across backends.
void   set_arena(void*, size_t) { }
size_t arena_available()        { return 0; }
size_t arena_size()             { return 0; }

} // namespace clockwork::mem

#else  // single-region platforms (desktop / WASM / NIF)

#include <atomic>
#include <new>

namespace clockwork::mem {
namespace {

/*
 * The arena, when a host has supplied one.
 *
 * An implicit doubly-linked list of blocks covering the whole span, first fit,
 * splitting on allocation and coalescing both ways on free. That is more than
 * the traffic needs — the engine's RT pool and clockwork heap's initial area
 * are a handful of large, long-lived allocations made at boot — but a real
 * allocator is what makes the arena a drop-in for malloc, and a drop-in is the
 * point: every caller keeps the code it already had.
 *
 * NOT for the audio thread. Nothing here allocates during render: the pool and
 * the heap take their areas at dsp_new and give them back at teardown, and the
 * growth path that could have run later is 0-sized off-device. The spin lock
 * costs nothing uncontended and keeps two control threads from interleaving a
 * split with a coalesce.
 */
struct alignas(16) Block {
    Block*   prev;
    Block*   next;
    size_t   size;   // payload bytes, excluding this header
    uint32_t free;   // 1 = available
    uint32_t magic;
};
static_assert(sizeof(Block) % 16 == 0,
              "header must be a whole number of 16-byte slots, so payloads stay aligned");

constexpr uint32_t kBlockMagic = 0x4152454E; // 'AREN'
constexpr size_t   kAlign      = 16;

Block*  g_head       = nullptr;
uint8_t* g_arena     = nullptr;
size_t  g_arena_size = 0;
size_t  g_arena_used = 0;

std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct Guard {
    Guard()  { while (g_lock.test_and_set(std::memory_order_acquire)) { } }
    ~Guard() { g_lock.clear(std::memory_order_release); }
};

constexpr size_t round_up(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

// Merge b with its successor when both are free. Leaves b as the survivor.
void coalesce_forward(Block* b) {
    Block* n = b->next;
    if (!n || !b->free || !n->free)
        return;
    if (reinterpret_cast<uint8_t*>(b) + sizeof(Block) + b->size
        != reinterpret_cast<uint8_t*>(n))
        return;                                  // not adjacent: never merge across a gap
    b->size += sizeof(Block) + n->size;
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    n->magic = 0;
}

} // namespace

void set_arena(void* base, size_t bytes) {
    Guard g;
    g_head = nullptr;
    g_arena = nullptr;
    g_arena_size = 0;
    g_arena_used = 0;
    if (!base || bytes < sizeof(Block) + kAlign)
        return;                                  // cleared, or too small to hold anything

    // Align the base up and lose whatever that costs from the far end.
    auto raw = reinterpret_cast<uintptr_t>(base);
    auto aligned = (raw + kAlign - 1) & ~(uintptr_t)(kAlign - 1);
    const size_t lost = static_cast<size_t>(aligned - raw);
    if (bytes <= lost + sizeof(Block) + kAlign)
        return;

    g_arena = reinterpret_cast<uint8_t*>(aligned);
    g_arena_size = (bytes - lost) & ~(size_t)(kAlign - 1);

    g_head = new (g_arena) Block{};
    g_head->prev  = nullptr;
    g_head->next  = nullptr;
    g_head->size  = g_arena_size - sizeof(Block);
    g_head->free  = 1;
    g_head->magic = kBlockMagic;
}

size_t arena_size() { return g_arena_size; }

size_t arena_available() {
    if (!g_head)
        return 0;
    Guard g;
    size_t total = 0;
    for (Block* b = g_head; b; b = b->next)
        if (b->free)
            total += b->size;
    return total;
}

// std::malloc already returns max_align_t-aligned memory (16 on 64-bit targets),
// satisfying the alignment contract; both tiers map to it with no wrapper. There
// is no second tier to spill to, so allow_spill is irrelevant.
void* alloc(Tier, size_t bytes, bool /*allow_spill*/) {
    if (bytes == 0)
        return nullptr;
    if (!g_head)
        return std::malloc(bytes);

    Guard g;
    const size_t need = round_up(bytes);
    for (Block* b = g_head; b; b = b->next) {
        if (!b->free || b->size < need)
            continue;
        // Split only when the tail can hold a header plus a usable payload;
        // otherwise the remainder goes to this allocation rather than becoming
        // a block nothing can ever be placed in.
        if (b->size >= need + sizeof(Block) + kAlign) {
            auto* tail = reinterpret_cast<Block*>(
                reinterpret_cast<uint8_t*>(b) + sizeof(Block) + need);
            tail->prev  = b;
            tail->next  = b->next;
            tail->size  = b->size - need - sizeof(Block);
            tail->free  = 1;
            tail->magic = kBlockMagic;
            if (b->next)
                b->next->prev = tail;
            b->next = tail;
            b->size = need;
        }
        b->free = 0;
        g_arena_used += b->size;
        return reinterpret_cast<uint8_t*>(b) + sizeof(Block);
    }
    return nullptr;   // the arena is bounded: a request that does not fit fails here
}

void free(void* ptr) {
    if (!ptr)
        return;
    if (!g_head) {
        std::free(ptr);
        return;
    }
    auto* p = static_cast<uint8_t*>(ptr);
    if (p < g_arena || p >= g_arena + g_arena_size) {
        std::free(ptr);   // taken before the arena was set — release it the way it came
        return;
    }
    Guard g;
    auto* b = reinterpret_cast<Block*>(p - sizeof(Block));
    if (b->magic != kBlockMagic || b->free)
        return;           // foreign or already free: do nothing rather than corrupt the list
    b->free = 1;
    g_arena_used -= b->size;
    coalesce_forward(b);
    if (b->prev && b->prev->free)
        coalesce_forward(b->prev);
}

size_t in_use(Tier) { return g_head ? g_arena_used : 0; }

size_t largest_free(Tier) {
    if (!g_head)
        return SIZE_MAX;  // no arena: single region, nothing to bound by
    Guard g;
    size_t best = 0;
    for (Block* b = g_head; b; b = b->next)
        if (b->free && b->size > best)
            best = b->size;
    return best;
}

size_t spill_count() { return 0; }

} // namespace clockwork::mem

#endif
