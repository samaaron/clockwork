// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// See clockwork_asset_pool.h. A sorted list of live slots; free space is what
// lies between them. First fit; freeing is removing a slot, which is what
// makes neighbours coalesce for nothing.
#include "clockwork_asset_pool.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>

namespace {
constexpr uint32_t kAlign = 16;
inline uint32_t roundUp(uint32_t v) { return (v + (kAlign - 1)) & ~(kAlign - 1); }
struct Slot { uint32_t start; uint32_t bytes; };
}

struct ClockworkAssetPool {
    uint32_t          size = 0;
    std::vector<Slot> live;   // sorted by start
};

ClockworkAssetPool* clockwork_asset_pool_open(uint32_t bytes) {
    if (bytes == 0) return nullptr;
    auto* p = new (std::nothrow) ClockworkAssetPool();
    if (!p) return nullptr;
    p->size = bytes;
    return p;
}

void clockwork_asset_pool_close(ClockworkAssetPool* pool) { delete pool; }

int clockwork_asset_pool_alloc(ClockworkAssetPool* pool, uint32_t bytes, uint32_t* offset_out) {
    if (!pool || !offset_out || bytes == 0) return 1;
    // 64-bit for the rounding and the cursor: a request near 4 GB must not
    // wrap into a "fit".
    const uint64_t need = (static_cast<uint64_t>(bytes) + (kAlign - 1)) & ~uint64_t(kAlign - 1);
    uint64_t cursor = 0;
    size_t   at = 0;
    for (; at < pool->live.size(); ++at) {
        const Slot& s = pool->live[at];
        if (s.start >= cursor + need) break;          // fits before this slot
        cursor = roundUp(s.start + s.bytes);          // else move past it
    }
    if (cursor + need > pool->size) return 1;
    pool->live.insert(pool->live.begin() + static_cast<std::ptrdiff_t>(at),
                      Slot{ static_cast<uint32_t>(cursor), bytes });
    *offset_out = static_cast<uint32_t>(cursor);
    return 0;
}

int clockwork_asset_pool_free(ClockworkAssetPool* pool, uint32_t offset) {
    if (!pool) return 1;
    auto it = std::lower_bound(pool->live.begin(), pool->live.end(), offset,
                               [](const Slot& s, uint32_t o) { return s.start < o; });
    if (it == pool->live.end() || it->start != offset) return 1;
    pool->live.erase(it);
    return 0;
}

uint32_t clockwork_asset_pool_free_bytes(const ClockworkAssetPool* pool) {
    if (!pool) return 0;
    uint64_t used = 0;
    for (const Slot& s : pool->live) used += (static_cast<uint64_t>(s.bytes) + (kAlign - 1)) & ~uint64_t(kAlign - 1);
    return used >= pool->size ? 0u : static_cast<uint32_t>(pool->size - used);
}
