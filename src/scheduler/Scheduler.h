// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron


    Generic real-time timed-event scheduler.

    Stores opaque event payloads keyed by an int64 timetag and releases them in
    time order. Each event carries a caller-defined Meta value and a tag used for
    selective cancellation. The structure is RT-safe: a fixed metadata pool, a
    shared bump-allocated data pool with in-place compaction, and a binary
    min-heap of (timetag, slot index) entries. No allocation, no locks.

    Payload bytes and Meta are opaque to the scheduler; what an event means and
    how it is delivered when due is entirely the caller's concern.

    ── Where the implementation lives ───────────────────────────────────────────

    In Rust: `rust/clockwork-schedule`, reached
    through the C ABI in `rust/clockwork-schedule/cpp/clockwork_schedule.h`. What is
    left here is the template — because the template is the part that cannot
    cross a C ABI. It carries two things Rust cannot be given:

      * `Meta`, a C++ type the store must reserve room for, address by slot and
        hand back typed, without ever being able to name it. The ABI takes its
        size and alignment; `add` constructs the caller's object in the storage
        the store reserved, and `popDue` hands the same address back as a
        `const Meta*`. The store itself never reads a byte of it.
      * `sched_tag_hash`, which must stay `constexpr`: test_scheduler.cpp asserts
        its results in `static_assert`, and no call across a C ABI can satisfy
        one. It is therefore spelled twice, here and in
        `clockwork-schedule/src/tag.rs`, and both are pinned to the same frozen
        values — see that file for why that is the right shape and not a leak.

    Everything else — the slot pool, the heap, the data pool and its compaction,
    the tag flush and the clear handshake — is on the other side of the ABI.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#include "clockwork_schedule.h"

// Stable 32-bit hash of a tag string (FNV-1a). Shared by producers and the
// flush side so the same string keys the same events. Never returns 0 — 0 is
// reserved as the flush wildcard ("cancel everything").
constexpr uint32_t sched_tag_hash(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= static_cast<uint8_t>(s[i]); h *= 16777619u; }
    return h ? h : 1u;
}

// Default tag for user-scheduled events; cancelled on a run stop.
constexpr uint32_t SCHED_TAG_DEFAULT = sched_tag_hash("default", 7);
// Graph (synth) events — a distinct tag so an outbound default-tag flush leaves
// them untouched; they are cleared via the wildcard flush(0) / clear().
constexpr uint32_t SCHED_TAG_SYNTH = sched_tag_hash("synth", 5);

template <typename Meta, int SlotCount, int DataPoolSize>
class Scheduler {
    static_assert(SlotCount > 0 && SlotCount <= 32767,
                  "SlotCount must be positive and fit the int16_t slot index");
    static_assert(DataPoolSize > 0, "DataPoolSize must be positive");
    // The store reserves Meta-shaped storage and addresses it by slot; it never
    // copies or destroys one. So a Meta must be safe to leave unwound when its
    // slot is reused, and cheap to construct in place.
    static_assert(std::is_trivially_destructible<Meta>::value,
                  "Meta must be trivially destructible: a released slot is "
                  "reused without running a destructor");

public:
    // A due event handed to the caller. Valid until release() is called for it.
    struct Event {
        int64_t        when = 0;
        uint32_t       tag  = 0;
        const Meta*    meta = nullptr;
        const uint8_t* data = nullptr;
        uint32_t       size = 0;
        int            slot = -1;   // opaque; pass back to release()
        bool valid() const { return slot >= 0; }
    };

    /*
     * THE QUEUE LIVES IN A STATIC ARENA, NOT ON THE HEAP.
     *
     * Allocating it (clockwork_sched_new) from a file-scope static runs during
     * static initialisation, before the heap is arranged, and fails. A null
     * handle then refuses every add exactly as a full queue does, so a scheduler
     * that was never built reports itself as a busy one and schedules nothing
     * for the life of the process, silently.
     *
     * The arena is sized generously at compile time and checked against the
     * real figure at construction: the Rust layout is not visible from here,
     * so guessing it would be two places computing one thing. 128 bytes a
     * slot covers the slot record, the queue entry, the scratch entry and the
     * meta, which together measure well under that.
     *
     * This is the same trade the ring buffers already make — address space in
     * the data segment, in exchange for memory that cannot fail to exist.
     */
    static constexpr size_t kArenaBytes =
        static_cast<size_t>(DataPoolSize) + static_cast<size_t>(SlotCount) * 128u + 4096u;

    Scheduler()
        : mStore(clockwork_sched_new_in_place(mArena, kArenaBytes,
                                        static_cast<uint32_t>(SlotCount),
                                        static_cast<uint32_t>(DataPoolSize),
                                        static_cast<uint32_t>(sizeof(Meta)),
                                        static_cast<uint32_t>(alignof(Meta)))) {}

    // Nothing to free: the arena is ours and static.
    ~Scheduler() = default;

    // The store owns pools addressed by raw pointer; copying the handle would
    // give two schedulers one queue — the copy's handle would still point into
    // the original's arena. (The C++ this replaced held a std::atomic member
    // and was non-copyable for the same practical reason, so no call site
    // loses anything.)
    Scheduler(const Scheduler&)            = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Store `data`/`size` to fire at timetag `when`, carrying `meta` and keyed by
    // `tag`. Returns false (no state change) if the slot pool or data pool is
    // full. RT-safe; no allocation.
    /*
     * Did the store actually build?
     *
     * clockwork_sched_new_in_place returns null when the arena is too small for the
     * shape, and every call on a null handle then answers the empty thing —
     * add refuses, next_time says i64::MAX. That is indistinguishable from a
     * queue that is merely full, which is a legitimate overload state, so a
     * scheduler that was never built reports itself as a busy one. It did
     * exactly that for the whole life of the web build. Anything that counts
     * a refusal should say which of the two it is.
     */
    bool ready() const { return mStore != nullptr; }

    /* What the arena would have to be for this shape, for a diagnostic that
       wants to say by how much it fell short. */
    static size_t bytesNeeded() {
        return clockwork_sched_bytes_needed(static_cast<uint32_t>(SlotCount),
                                      static_cast<uint32_t>(DataPoolSize),
                                      static_cast<uint32_t>(sizeof(Meta)),
                                      static_cast<uint32_t>(alignof(Meta)));
    }

    bool add(int64_t when, uint32_t tag, const Meta& meta,
             const uint8_t* data, uint32_t size) {
        void* slot = clockwork_sched_add(mStore, when, tag, data, size);
        if (!slot) return false;
        // Construct the caller's record in the storage the store reserved.
        // Placement new, so this allocates nothing and the object handed back
        // by popDue is a real Meta and not a reinterpreted pointer.
        ::new (slot) Meta(meta);
        return true;
    }

    // Timetag of the earliest live event, or INT64_MAX if none.
    int64_t nextTime() const { return clockwork_sched_next_time(mStore); }

    // Pop the earliest event if it is due at/through `now`. The returned Event
    // borrows the data pool; call release(event) once the caller is done with it.
    // An invalid Event (valid() == false) means nothing is due.
    Event popDue(int64_t now) {
        ClockworkSchedEvent e;
        if (!clockwork_sched_pop_due(mStore, now, &e)) return Event{};
        return Event{ e.when, e.tag, static_cast<const Meta*>(e.meta),
                      e.data, e.size, static_cast<int>(e.slot) };
    }

    // Return a popped event's slot to the pool. When the queue empties, the data
    // pool resets to zero (zero-cost compaction).
    void release(const Event& e) { clockwork_sched_release(mStore, e.slot); }

    // Cancel every live event whose tag matches `tag` (tag 0 = all). Matching
    // slots are freed and the heap is rebuilt from the survivors, so the heap
    // never carries dead entries. RT-safe (no allocation); O(n) over the pool.
    void flush(uint32_t tag) { clockwork_sched_flush(mStore, tag); }

    void clear() { clockwork_sched_clear(mStore); }

    int      size() const { return clockwork_sched_size(mStore); }
    bool     full() const { return clockwork_sched_full(mStore) != 0; }
    uint32_t dataUsed() const { return clockwork_sched_data_used(mStore); }
    uint32_t dataCapacity() const { return static_cast<uint32_t>(DataPoolSize); }

    // Cross-thread clear handshake: clear() is not safe to call concurrently with
    // the time-ordered operations on the audio thread. A control thread calls
    // requestClear() (lock-free); the audio thread calls drainPendingClear() at a
    // safe point. Release/acquire pairs the request with the drain.
    void requestClear() { clockwork_sched_request_clear(mStore); }
    bool drainPendingClear() { return clockwork_sched_drain_pending_clear(mStore) != 0; }

private:
    /*
     * Per instance, not static: a shared arena would give two schedulers of
     * the same shape one queue. Declared before mStore because members
     * initialise in declaration order and the constructor hands this to
     * clockwork_sched_new_in_place.
     *
     * A file-scope or function-local static Scheduler therefore carries its
     * queue in BSS — which is exactly where the C++ scheduler this replaced
     * kept its storage, and why that one could not fail to exist.
     */
    alignas(16) uint8_t mArena[kArenaBytes];
    ClockworkSched* mStore;
};
