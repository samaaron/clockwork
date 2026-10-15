// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_pool.h — the guest's pool: clockwork's allocator, run over the
 * guest's bulk memory.
 *
 * A guest gets two spans from clockwork. DspConfig::memory, the fast one, it
 * lays out by hand: its hot, per-block state. DspConfig::memory_bulk it
 * never touches by hand: clockwork runs this allocator over the whole of it,
 * and everything the guest allocates as it runs — a buffer a client asked
 * for empty, a definition, a table, a Rust guest's every Box and Vec — comes
 * from there. A C guest reaches it through DspHost::alloc_bytes and
 * free_bytes. A Rust guest reaches it without knowing: clockwork's Rust
 * global allocator (rust/clockwork-heap/src/router.rs) serves every
 * allocation made while a guest entry point is open from this pool, through
 * the hooks installed at boot (clockwork_router_guest_hooks).
 *
 * The pool is the segregated-fit pool in rust/clockwork-heap, operated
 * lock-free by the engine thread, with frees from any other thread parked
 * on a lock-free stack the engine drains, and a census (bytes in use, the
 * most ever) that reaches the metrics. The law (2026-08-21): no locks of
 * any kind on the audio thread, and no malloc there beyond our own arena
 * management.
 *
 * THE POOL NEVER GROWS. The guest declares what it needs and gets exactly
 * that (dsp_api.h); a pool that fills answers NULL, loudly, and the fix is
 * the guest's declaration or the host's floor. Until 2026-10-15 a spare
 * block was kept armed so the pool could grow once; it was a hedge against
 * the declaration being wrong, sat oddly beside a model in which the
 * declaration is the budget, and cost a block, an area, a knob and a
 * growth path. Gone.
 *
 * Until 2026-10-15 this was clockwork's own heap, sized by clockwork and
 * named as clockwork's; the guest's memory was then in two places, one the
 * guest carved and one clockwork allocated. Now the span is the guest's,
 * sized by what the guest declares, and clockwork only runs the allocator.
 *
 * Also here, because its block is taken beside the guest's: the RT router's
 * block, the one clockwork's own Rust subsystems allocate from while an
 * engine entry point is open. The process's for life.
 */

#pragma once
#include <cstddef>
#include <cstdint>

/* Boot, on the control thread, with the guest's bulk span carved: run the
 * allocator over [bulk, bulk + bytes) (NULL/0: no pool, and every
 * allocation answers NULL) and arm the RT router if it is not yet (its
 * block is taken once for the life of the process). Every build; a rebuild
 * starts the pool over. */
void clockwork_pool_boot(void* bulk, size_t bytes);

/* Take bytes from the pool. Any thread: on the engine thread straight from
 * the pool, lock-free; on any other thread from the system allocator, on
 * that thread, and routed back on free by address. NULL when the pool is
 * full, or for 0 bytes. */
void* clockwork_pool_alloc(size_t bytes);

/* Give bytes back. Any thread: a foreign thread's free is parked and the
 * engine drains it on its next allocation or free. A pointer the pool does
 * not hold is treated as system memory (a foreign thread's allocation
 * above). NULL is ignored. */
void clockwork_pool_free(void* ptr);

/* Whether the pool holds this address: inside its span. Lock-free, any
 * thread. */
bool clockwork_pool_holds(const void* ptr);

/* Forget the pool: the span is the guest's again. destroy_dsp. */
void clockwork_pool_teardown();

/* Forget the pool without freeing anything: it was a dead engine's, placed
 * in an arena about to be started over (clockwork::mem::reset_arena), so
 * its memory is the arena's to hand out again. The web's reload. */
void clockwork_pool_abandon();

/* The census: payload bytes in use now, and the most ever. */
size_t clockwork_pool_used_bytes();
size_t clockwork_pool_peak_bytes();

/* The engine thread claims pool ownership (called at boot and each block). */
void clockwork_pool_register_engine_thread();
/* Foreign-thread maintenance: releases the system pointers the engine
 * thread parked. Called from loader/egress threads on native; on
 * single-threaded targets it is safely callable from anywhere. */
void clockwork_pool_foreign_maintenance();

/* ── The RT router ──────────────────────────────────────────────────────── */

/* The router's block: what clockwork's Rust subsystems allocate inside an
 * engine entry point (a sink's or a port's queues) comes from here, not
 * from the system allocator. Taken once (CLOCKWORK_ROUTER_BYTES, fast tier)
 * for the life of the process and never freed: the queues outlive any one
 * engine. Begin is 0 and bytes 0 while the router is unarmed. */
uintptr_t clockwork_router_begin();
size_t    clockwork_router_bytes();
/* Take the router's block now (a host that wants it first in its arena, so
 * a start-over finds it in the same place); clockwork_pool_boot does the
 * same if no one has. */
void      clockwork_router_reserve();
/* After an arena the block lives in started over: take the block again and
 * say whether it came back where it was. True when it did, or when none was
 * ever armed. */
bool      clockwork_router_reclaim();
/* The router's census (rust/clockwork-heap/src/router.rs): live payload
 * bytes in its block, and allocations that fell back to the system
 * allocator because the block was full (a sizing bug made visible, never
 * silent). */
extern "C" size_t   clockwork_router_used_bytes(void);
extern "C" uint32_t clockwork_router_fallback_allocs(void);
/* The same for the guest's pool, as the router saw it: Rust allocations
 * inside a guest entry point that the pool could not meet and the system
 * allocator served instead. */
extern "C" uint32_t clockwork_router_guest_fallback_allocs(void);
