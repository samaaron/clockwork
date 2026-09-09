// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * timeline_mirror.h — a ClockworkTimeline handed across a thread or process boundary
 * without a lock.
 *
 * The session clock reaches the plugin bridge as a ClockworkClockState copied into
 * the shared segment each block (plugin_bridge.h mirror_clock). A midi
 * follower timeline has no such state: it is a snapshot, and the registry
 * that makes it is behind a mutex the audio thread may not take. So the
 * engine's audio thread reads each slot's lock-free published copy
 * (clockwork_midi_timelines_timeline_rt) and writes it here, under a seqlock, and
 * the bridge's audio thread reads it back the same way — one slot per
 * timeline id, in the segment header. The bytes are the struct's own,
 * carried as 64-bit words so no field can tear.
 *
 * Writer: one at a time (the engine's audio thread). Reader: the bridge's
 * audio thread, which retries a torn read a bounded number of times and
 * then reports failure rather than spin against a writer that is mid-way.
 */
#pragma once

#include "clockwork_clock.h"

#include <atomic>
#include <cstdint>
#include <cstring>

constexpr uint32_t kClockworkTimelineMirrorWords = sizeof(ClockworkTimeline) / sizeof(uint64_t);
static_assert(sizeof(ClockworkTimeline) == kClockworkTimelineMirrorWords * sizeof(uint64_t),
              "a ClockworkTimeline is carried as whole 64-bit words");

// Named outside the namespace so plugin_track.h, which is C, can declare it.
struct alignas(8) ClockworkTimelineMirrorSlot {
    std::atomic<uint32_t> seq{0};    // odd while a write is in progress
    uint32_t              _pad{0};
    std::atomic<uint64_t> w[kClockworkTimelineMirrorWords];
};
static_assert(sizeof(ClockworkTimelineMirrorSlot) == 8 + sizeof(ClockworkTimeline),
              "ClockworkTimelineMirrorSlot is laid out by hand: it lives in shared memory");

namespace clockwork {

inline void timelineMirrorWrite(ClockworkTimelineMirrorSlot& s, const ClockworkTimeline& t) {
    uint64_t words[kClockworkTimelineMirrorWords];
    std::memcpy(words, &t, sizeof words);
    const uint32_t s0 = s.seq.load(std::memory_order_relaxed);
    s.seq.store(s0 + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (uint32_t i = 0; i < kClockworkTimelineMirrorWords; ++i)
        s.w[i].store(words[i], std::memory_order_relaxed);
    s.seq.store(s0 + 2, std::memory_order_release);
}

// Retries before a read gives up on a slot being written.
constexpr int kTimelineMirrorReadTries = 16;

// True and `out` filled on a clean read; false, `out` untouched, when the
// writer was mid-way through every try. A slot nothing has written reads as
// all zeros — a ClockworkTimeline with id 0 and no tempo — which the caller must
// treat as "nothing there", as it treats id -1.
inline bool timelineMirrorRead(const ClockworkTimelineMirrorSlot& s, ClockworkTimeline& out) {
    for (int tries = 0; tries < kTimelineMirrorReadTries; ++tries) {
        const uint32_t s1 = s.seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        uint64_t words[kClockworkTimelineMirrorWords];
        for (uint32_t i = 0; i < kClockworkTimelineMirrorWords; ++i)
            words[i] = s.w[i].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.seq.load(std::memory_order_relaxed) != s1) continue;
        std::memcpy(&out, words, sizeof out);
        return true;
    }
    return false;
}

}  // namespace clockwork
