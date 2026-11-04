// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_heap.h — a core's heap (rust/clockwork-heap/src/core_heap.rs;
 * docs/ENGINES.md): two pools, each clockwork's
 * allocator run over one span of the core's memory. One heap per core
 * (clockwork_core_heap).
 *
 *   GUEST  over the guest's bulk memory: everything the guest allocates as it
 *          runs — a C guest's through DspHost::alloc_bytes, a Rust guest's
 *          every Box and Vec through clockwork's global allocator, which
 *          reaches the pool through the thread's entry.
 *   OWN    over a block the core takes at its first init: what clockwork's
 *          Rust subsystems make inside a door (a sink's or a port's queues).
 *
 * A pool is the thread's that holds the core's gate (core_state.h): only
 * that thread takes from it, and a block any other thread gives back is
 * parked until the holder's next allocation or free. While a pool runs, its
 * memory is in the address map, so a free on any thread finds the pool by
 * the block's address. A pool never grows: what it cannot meet answers
 * NULL, and the global allocator counts it.
 *
 * Nothing outlives its core. The core's own pool goes when the core does,
 * after its sinks and ports, and must be empty then: a core that went with
 * something still in it is counted (clockwork_heap_outlived), its span is
 * kept rather than given back under it, and a free that reaches the span
 * later is dropped and counted (clockwork_heap_outlived_frees).
 *
 * Also the core's parked system frees: memory a thread without the gate took
 * from the host's own allocator for the guest, given back during a block,
 * where freeing it would be a call into the system allocator on the audio
 * thread. Released by the next call into the core that is not a block.
 *
 * Pure C. Every call takes the heap and the pool; NULL is no heap.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkHeap ClockworkHeap;   /* opaque: clockwork_heap::core_heap::CoreHeap */

/* The address map's grain: a pool's memory is found by the 64 KiB chunks
 * it touches, and no two cores' memory may share one. A process that holds
 * several cores takes each core's memory in whole chunks on a chunk.
 * (rust/clockwork-heap/src/map.rs, CHUNK_BYTES) */
#define CLOCKWORK_HEAP_CHUNK_BYTES 65536u

typedef enum ClockworkPool {
    CLOCKWORK_POOL_GUEST = 0,
    CLOCKWORK_POOL_OWN   = 1
} ClockworkPool;

/* A pool's census: payload bytes in use, the most ever, the frees of a block
 * already free it refused (a caller's bug made visible), and the
 * allocations it could not meet, which the system served. */
typedef struct ClockworkPoolCensus {
    size_t   used;
    size_t   peak;
    uint32_t double_frees;
    uint32_t fallbacks;
} ClockworkPoolCensus;

/* What a walk of a pool found: code 0 when it is whole, else the first
 * thing found wrong (1 a chain ran past its area, 2 a block size, 3 a
 * boundary tag, 4 adjacent free blocks, 5 a free-list entry not free, 6 a
 * free list and its bitmap disagree, 7 a free block missing from or doubled
 * in the index); with it, the bad block's header, and the payload and size
 * of the block before it, the allocation that most likely overran. */
typedef struct ClockworkPoolCheck {
    int32_t code;
    size_t  bad;
    size_t  prev_payload;
    size_t  prev_size;
} ClockworkPoolCheck;

/* Run a pool over [span, span + bytes), 16-aligned; `give_back`, when not
 * NULL, returns the span when the core goes (NULL: its lender keeps it). The
 * gate's holder, never the audio thread. A pool already running goes first,
 * and what was parked for it with it; its span stays its lender's. False,
 * with no pool, when the span cannot hold the pool's headers or shares a
 * 64 KiB chunk with another core's memory. */
bool clockwork_heap_start(const ClockworkHeap* heap, ClockworkPool pool, void* span, size_t bytes,
                          void (*give_back)(void* span));

/* Stop a pool: its memory leaves the address map and stays its lender's, and
 * what was parked for it goes with it. The gate's holder. */
void clockwork_heap_stop(const ClockworkHeap* heap, ClockworkPool pool);

/* Whether a pool is running, and its span (NULL when it is not), with the
 * span's length through `bytes` when that is not NULL. Any thread. */
bool  clockwork_heap_running(const ClockworkHeap* heap, ClockworkPool pool);
void* clockwork_heap_span(const ClockworkHeap* heap, ClockworkPool pool, size_t* bytes);

/* Bytes from a pool: NULL when it is full, not running, asked for 0, or this
 * thread does not hold the core's gate. */
void* clockwork_heap_take(const ClockworkHeap* heap, ClockworkPool pool, size_t bytes);

/* Give back a block of a pool: freed now by the gate's holder, parked from
 * any other thread. False, and the pointer left alone, when it is not the
 * pool's. */
bool clockwork_heap_give(const ClockworkHeap* heap, ClockworkPool pool, void* p);

/* Whether a pool holds this address. Any thread. */
bool clockwork_heap_holds(const ClockworkHeap* heap, ClockworkPool pool, const void* p);

/* A pool's census, and a walk of the whole of it: the gate's holder. */
ClockworkPoolCensus clockwork_heap_census(const ClockworkHeap* heap, ClockworkPool pool);
ClockworkPoolCheck  clockwork_heap_check(const ClockworkHeap* heap, ClockworkPool pool);

/* Park memory from the host's allocator that a block gave back (dead to its
 * owner, at least 16 bytes); release what is parked, each through `free`,
 * off the audio thread. A NULL `free` forgets them: memory going whole. */
void clockwork_heap_park_system(const ClockworkHeap* heap, void* p);
void clockwork_heap_release_system(const ClockworkHeap* heap, void (*free)(void*));

/* Cores that went with clockwork memory still in their own pool, and blocks
 * of such a pool freed after the core went: the span is kept and the free is
 * dropped and counted, never handed to the system allocator. The tests
 * require none of either. */
uint32_t clockwork_heap_outlived(void);
uint32_t clockwork_heap_outlived_frees(void);

#ifdef __cplusplus
}
#endif
