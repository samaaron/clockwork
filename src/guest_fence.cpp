// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// guest_fence.cpp — see guest_fence.h.
#include "guest_fence.h"
#include "clockwork_arena.h"

#if defined(__SANITIZE_ADDRESS__)
#define CLOCKWORK_FENCE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CLOCKWORK_FENCE_ASAN 1
#endif
#endif
#ifndef CLOCKWORK_FENCE_ASAN
#define CLOCKWORK_FENCE_ASAN 0
#endif

#if CLOCKWORK_FENCE_ASAN
#include <sanitizer/asan_interface.h>
#include "core_state.h"   // the entered core's arena

namespace {

// The harness's, for the process: whether to fence at all.
bool g_armed = false;
// Each thread's: the arena whose regions it has poisoned, while the guest
// call it is inside is open.
thread_local const uint8_t* t_poisoned = nullptr;

// What the guest may touch: a region it owns, one it is in the audience of,
// and the IN ring, because the message a guest is handed (dsp_osc) is read
// in place from there.
bool guestMayTouch(const ClockworkArenaEntry& e) {
    return e.owner == CLOCKWORK_OWNER_GUEST || (e.audience & CLOCKWORK_AUDIENCE_GUEST) != 0
        || e.id == CLOCKWORK_ARENA_CLOCKWORK_IN_RING;
}

void setPoison(const uint8_t* base, bool on) {
    const auto* h = reinterpret_cast<const ClockworkArenaHeader*>(base);
    if (!h || h->magic != CLOCKWORK_ARENA_MAGIC || h->state != CLOCKWORK_ARENA_PUBLISHED) return;
    for (uint32_t i = 0; i < h->entry_count && i < CLOCKWORK_ARENA_MAX_ENTRIES; ++i) {
        const ClockworkArenaEntry& e = h->entries[i];
        if (e.id == 0 || e.bytes == 0 || guestMayTouch(e)) continue;
        if (e.offset > h->arena_bytes || e.bytes > h->arena_bytes - e.offset) continue;
        if (on) __asan_poison_memory_region(base + e.offset, e.bytes);
        else    __asan_unpoison_memory_region(base + e.offset, e.bytes);
    }
}

}  // namespace

void clockwork_guest_fence_arm(bool armed) {
    if (!armed && t_poisoned) { setPoison(t_poisoned, false); t_poisoned = nullptr; }
    g_armed = armed;
}

void clockwork_guest_fence_close() {
    if (!g_armed || t_poisoned) return;
    const uint8_t* base = clockwork::laid_out_base(clockwork_core_entry().core);
    if (!base) return;
    setPoison(base, true);
    t_poisoned = base;
}

void clockwork_guest_fence_open() {
    if (!t_poisoned) return;
    setPoison(t_poisoned, false);
    t_poisoned = nullptr;
}

HostCallScope::HostCallScope() {
    if (!t_poisoned) return;
    setPoison(t_poisoned, false);
    mLifted = t_poisoned;
    t_poisoned = nullptr;
}

HostCallScope::~HostCallScope() {
    if (!mLifted || !g_armed) return;
    setPoison(mLifted, true);
    t_poisoned = mLifted;
}

#else  // not an AddressSanitizer build: nothing to fence with

void clockwork_guest_fence_arm(bool) {}
void clockwork_guest_fence_close() {}
void clockwork_guest_fence_open() {}
HostCallScope::HostCallScope() {}
HostCallScope::~HostCallScope() {}

#endif
