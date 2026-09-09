// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * mem_region.h — tiered memory placement for clockwork
 *
 * clockwork runs on devices with one kind of RAM (desktop, WASM) and on
 * devices with two (ESP32-S3: fast internal SRAM + slower bulk PSRAM). This lets
 * allocation sites express *intent* — "this is real-time hot" vs "this is
 * cold/large" — and leaves the mapping of intent to physical memory to a
 * per-platform backend.
 *
 *   Tier::Fast — low-latency memory for the audio-thread-hot data (clockwork
 *                heap's initial area, audio buses, wire scratch).
 *                ESP32 → internal SRAM.
 *   Tier::Bulk — large, latency-tolerant memory for cold or late data (sample
 *                buffers, heap overflow). ESP32 → PSRAM.
 *
 * On single-region platforms (desktop, WASM, NIF) both tiers compile straight to
 * std::malloc / std::free with no wrapper — zero overhead and byte-for-byte the
 * behaviour these allocations had before. The tier is then purely documentation
 * of intent that an embedded build acts on.
 *
 * UNLESS THE HOST SUPPLIES AN ARENA. set_arena() points the single-region
 * backend at a fixed span the host owns, and every allocation then comes from
 * inside it rather than from the system.
 *
 * That is not a wasm special case, it is what wasm needs and nothing else has:
 * emscripten's malloc starts after the static data and grows upward into
 * whatever comes next, and what comes next is the guest's region. An
 * 8 MB real-time pool taken with malloc walks straight into it. The engine used
 * to dodge this by having its client pre-carve a block and pass the offset in
 * through a config-block slot, which the guest read out of two file-scope
 * externs — one allocation path on native and a different one on web, and the
 * web one bypassed this interface entirely.
 *
 * A host that calls set_arena() gets bounded allocation with no system calls;
 * a host that does not gets std::malloc exactly as before. The caller's code
 * is the same either way, which is the whole point.
 */
#pragma once
#include <cstddef>

namespace clockwork::mem {

enum class Tier { Fast = 0, Bulk = 1 };

// Point the single-region backend at a host-owned span. Every subsequent
// alloc() is served from inside [base, base+bytes) and every free() returns to
// it; nothing is ever handed back to the system, because the span was never
// taken from it. Call once, before any allocation, and never with a live
// allocation outstanding.
//
// Both tiers share the span: a single-region platform has one kind of memory
// by definition, so the tier stays what it is everywhere else here —
// documentation of intent. largest_free() reports the span's real largest free
// block rather than SIZE_MAX, so callers that size themselves against it (the
// heap's initial area, the engine's RT pool) bound themselves to what exists.
//
// base must be 16-byte aligned. Passing base=nullptr or bytes=0 clears the
// arena and restores std::malloc, which is what tests do between cases.
//
// No-op on tiered (ESP) builds, which have real regions to place into.
void set_arena(void* base, size_t bytes);

// Bytes of the arena not currently handed out, including free space stranded
// between live blocks; 0 when no arena is set. Coalescing means this is a
// budget, not a promise — compare largest_free() before a single big request.
size_t arena_available();

// Total size of the arena, or 0 when none is set.
size_t arena_size();

// Allocate at least 16-byte aligned (SC_MEMORY_ALIGNMENT) memory, or nullptr on
// failure.
//
// allow_spill=true (default): a request the named tier cannot satisfy falls back
// to the other region rather than failing outright. A Fast request landing in
// Bulk also increments spill_count().
//
// allow_spill=false: the request is served from the named tier only, returning
// nullptr if it does not fit. Callers that need Fast placement specifically (the
// heap's initial area) use this to notice the failure and pick their
// own fallback, rather than having a too-large request land wholesale in slow
// RAM with nothing reported.
void* alloc(Tier tier, size_t bytes, bool allow_spill = true);

// Release memory returned by alloc(). Tier-agnostic.
void free(void* ptr);

// Bytes currently handed out for a tier, for on-device SRAM budgeting / metrics.
// Single-region platforms don't track this (they have nothing to budget) and
// return 0.
size_t in_use(Tier tier);

// Largest contiguous free block in a tier's backing region, for sizing Fast areas
// at boot from the real (fragmented) SRAM map instead of a compile-time constant
// that can drift from the firmware's memory layout. Returns SIZE_MAX on
// single-region platforms, where any compile-time cap is lower and so wins.
size_t largest_free(Tier tier);

// Number of Fast allocations that transparently fell back to Bulk because Fast
// was exhausted. Non-zero means something meant for internal SRAM is in bulk RAM;
// hosts should surface it, as the symptom is usually render overruns rather than
// an error. Always 0 on single-region platforms.
size_t spill_count();

} // namespace clockwork::mem
