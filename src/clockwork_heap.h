// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_heap.h — the engine's buffer heap, one path on every target.
 *
 * A pre-claimed pool (rust/clockwork-heap) owned by the engine thread and
 * operated lock-free; foreign threads get system memory on their own thread
 * and their frees of pool memory are parked on a lock-free stack the engine
 * drains. Growth comes only from a spare area pre-armed off the audio
 * thread. The law (2026-08-21): no locks of any kind on the audio thread,
 * and no malloc there beyond our own arena management.
 *
 * WASM used to inline these to emscripten's malloc; that was a second
 * allocation story for the same engine, and a malloc on the audio thread.
 * Now the pool serves every target (sized per target in memory_profile.h).
 */

#pragma once
#include <cstddef>
#include <cstdint>

void   clockwork_heap_init(size_t bytes);
void*  clockwork_heap_alloc(size_t bytes);
void   clockwork_heap_free(void* ptr);
void   clockwork_heap_destroy();
/*
 * The first byte past the heap's initial backing block, or 0 before init.
 *
 * Clockwork's heap and the guest's region are adjacent
 * numbers in one address space with nothing between them, and on web the heap
 * is claimed with malloc — so its extent is not a number anyone chose. It
 * overran guestMemoryOffset by 3.08 MB on EVERY boot and nothing said so.
 * init_memory compares this against the guest region base and refuses to run
 * if they meet.
 */
uintptr_t clockwork_heap_backing_end();

size_t clockwork_heap_total_allocated();
size_t clockwork_heap_growth_count();

/* The engine thread claims pool ownership (called at init and each block). */
void   clockwork_heap_register_engine_thread();
/* Foreign-thread maintenance: releases engine-parked system pointers and
 * re-arms the growth spare. Called from loader/egress threads on native; on
 * single-threaded targets it is safely callable from anywhere. */
void   clockwork_heap_foreign_maintenance();
