/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2025-2026 Sam Aaron
 *
 * clockwork_schedule.h — the C ABI of rust/clockwork-schedule.
 *
 * Clockwork's timed-event store, and the two wire forms that carry a time.
 * The contract is src/scheduler/Scheduler.h and src/scheduler/schedule_parse.h;
 * this is the boundary between them and the Rust that implements them, and the C++
 * headers are thin templates over it so no call site had to move.
 *
 * Threading: one thread owns a store and makes every call on it. The single
 * exception is clockwork_sched_request_clear, which any thread may call and which the
 * owner performs at a safe point via clockwork_sched_drain_pending_clear.
 *
 * Real-time: clockwork_sched_new allocates; clockwork_sched_new_in_place does not, and
 * is what a host with a static arena should use. Nothing else here
 * allocates, locks, blocks or panics.
 */
#ifndef CLOCKWORK_SCHEDULE_H
#define CLOCKWORK_SCHEDULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque store. */
typedef struct ClockworkSched ClockworkSched;

/* A due event. `meta` and `data` borrow the store's pools and stay valid until
 * clockwork_sched_release is called for `slot`. */
typedef struct ClockworkSchedEvent {
    int64_t     when;
    uint32_t    tag;
    uint32_t    size;
    const void* meta;   /* the caller's record for this event, meta_size bytes */
    const uint8_t* data;
    int32_t     slot;   /* opaque; hand back to clockwork_sched_release */
} ClockworkSchedEvent;

/* Build a store. `meta_size`/`meta_align` describe the caller's per-event
 * record; the store never reads it, it only reserves and addresses it, which
 * is what lets Rust carry a C++ type it cannot name. Returns NULL for a
 * slot_count of 0 or above 32767, an empty data pool, a meta description that
 * is not a valid layout, or an allocation failure. */
ClockworkSched* clockwork_sched_new(uint32_t slot_count, uint32_t data_pool_size,
                        uint32_t meta_size, uint32_t meta_align);

/*
 * Bytes a caller must supply to clockwork_sched_new_in_place, or 0 for an invalid
 * shape. Computable before any scheduler exists, so a static array can be
 * sized against it at startup and the result checked.
 */
size_t clockwork_sched_bytes_needed(uint32_t slot_count, uint32_t data_pool_size,
                              uint32_t meta_size, uint32_t meta_align);

/*
 * Build a scheduler inside memory the caller owns. ALLOCATES NOTHING, so it
 * cannot fail for want of a heap.
 *
 * That is the point. clockwork_sched_new allocates, and a host that builds its
 * scheduler from a file-scope static runs that constructor during static
 * initialisation — before the heap is arranged. The allocation fails, this
 * returns null, and a null handle then refuses every add exactly as a full
 * queue does. A scheduler that was never built is indistinguishable from a
 * busy one, which is how the web build ran for its whole life with a
 * scheduler that accepted nothing.
 *
 * `mem` must be writable for `len` bytes and must outlive the scheduler.
 * Do not pass the result to clockwork_sched_free; the memory is not ours.
 */
ClockworkSched* clockwork_sched_new_in_place(uint8_t* mem, size_t len,
                                 uint32_t slot_count, uint32_t data_pool_size,
                                 uint32_t meta_size, uint32_t meta_align);
void      clockwork_sched_free(ClockworkSched* h);

/* Store `size` bytes to fire at `when` under `tag`. Returns this event's
 * metadata slot — the caller constructs its record there — or NULL if the slot
 * pool or the data pool is full, with no state changed. */
void*     clockwork_sched_add(ClockworkSched* h, int64_t when, uint32_t tag,
                        const uint8_t* data, uint32_t size);

/* Timetag of the earliest live event, or INT64_MAX if none. */
int64_t   clockwork_sched_next_time(const ClockworkSched* h);

/* Pop the earliest event if it is due at/through `now`. 1 and `*out` filled,
 * or 0 and `*out` untouched. */
int32_t   clockwork_sched_pop_due(ClockworkSched* h, int64_t now, ClockworkSchedEvent* out);

/* Return a popped event's slot. Negative, out-of-range and already-free slots
 * are no-ops. */
void      clockwork_sched_release(ClockworkSched* h, int32_t slot);

/* Cancel every live event carrying `tag`. Tag 0 is the wildcard. */
void      clockwork_sched_flush(ClockworkSched* h, uint32_t tag);
void      clockwork_sched_clear(ClockworkSched* h);

int32_t   clockwork_sched_size(const ClockworkSched* h);          /* live events */
/* 1 when the SLOT pool is full. The DATA pool can be exhausted while slots
 * remain free, and this does not report that — an add refusal is the only
 * complete backpressure signal. */
int32_t   clockwork_sched_full(const ClockworkSched* h);
uint32_t  clockwork_sched_data_used(const ClockworkSched* h);
uint32_t  clockwork_sched_data_capacity(const ClockworkSched* h);

/* The cross-thread clear handshake. */
void      clockwork_sched_request_clear(const ClockworkSched* h);
int32_t   clockwork_sched_drain_pending_clear(ClockworkSched* h);

/* FNV-1a over exactly `n` bytes, never 0 (0 is the flush wildcard).
 * src/scheduler/Scheduler.h keeps a constexpr twin because the C++ tests
 * assert it in static_assert, which no call across this ABI can satisfy; both
 * are pinned to the same frozen values. */
uint32_t  clockwork_sched_tag_hash(const uint8_t* s, size_t n);

/* ── The wire forms ─────────────────────────────────────────────────────── */

/* `ok` is 0/1 rather than a C++ bool so the ABI does not depend on how a given
 * compiler lays one out. `blob` borrows the caller's buffer. */
typedef struct ClockworkSchedulePacket {
    int32_t        ok;
    int64_t        when;
    const uint8_t* blob;
    uint32_t       blob_len;
} ClockworkSchedulePacket;

/* NTP seconds (since 1900) -> OSC 32.32 fixed-point timetag. */
int64_t   clockwork_sched_ntp_to_timetag(double ntp);

/* A timestamped OSC bundle: the "#bundle" marker AND a whole 8-byte timetag. */
int32_t   clockwork_sched_is_bundle(const uint8_t* data, uint32_t size);
/* That timetag, big-endian, from offset 8. Call clockwork_sched_is_bundle first. */
uint64_t  clockwork_sched_bundle_timetag(const uint8_t* bundle);

/* Parse "/clockwork/schedule <timetag> <blob>". The timetag is the OSC int64
 * 'h', or a 'd'/'f' NTP-seconds convenience. Every byte offset the parser uses
 * is derived from the reserved prefix, so it cannot fall out of step with it. */
ClockworkSchedulePacket clockwork_sched_parse(const uint8_t* msg, uint32_t size);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* CLOCKWORK_SCHEDULE_H */
