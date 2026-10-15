// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * guest_malloc.h — what the guest takes from the system allocator.
 *
 * dsp_api.h tells a guest where its memory is: fixed state in its working
 * memory, run-time allocation through alloc_bytes, nothing from malloc on the
 * audio thread. Nothing can stop a guest ignoring that — a malloc is a
 * function call — but clockwork can see it happen. The engine marks every
 * call into the guest (dsp_describe, dsp_new, dsp_process, dsp_osc, ...), and
 * where the link lets clockwork stand between the program and its allocator
 * (wasm-ld and GNU ld: --wrap=malloc), every malloc, calloc, realloc and free
 * made while a guest call is open is counted as the guest's.
 *
 * Two numbers come out, both in the metrics (shared_memory.h):
 *
 *   guest_malloc_bytes       what the guest holds from malloc now: the bytes of
 *                            everything it took inside its entry points, less
 *                            what it freed inside them. Sized by the
 *                            allocator's own answer (malloc_usable_size), so
 *                            the number is what the allocator gave, not what
 *                            was asked.
 *   guest_mallocs_processing malloc/calloc/realloc calls made inside
 *                            dsp_process, dsp_osc or dsp_asset since boot:
 *                            allocations on the audio thread, after boot. The
 *                            number a well-behaved guest keeps at 0.
 *
 * "Inside its entry points" is the whole of the rule: whatever runs while the
 * guest's call is open is the guest's, host callbacks included. The counters
 * are the engine thread's; the guest is only ever called from it.
 *
 * Where the link cannot wrap (Apple's ld), the wrappers are not linked, no
 * call is counted and both numbers stay 0. Nothing here changes what any
 * allocation does.
 */

#pragma once
#include <cstddef>
#include <cstdint>

// Which guest entry point is open, if any.
enum class GuestCall : uint8_t {
    None       = 0,
    Boot       = 1,   // dsp_describe, dsp_memory_bytes, dsp_memory_bulk_bytes, dsp_new, dsp_free
    Processing = 2,   // dsp_process, dsp_osc, dsp_asset
};

// Open and close a guest call. Not nested: the guest is called from one
// thread, one entry point at a time.
void clockwork_guest_call_begin(GuestCall kind);
void clockwork_guest_call_end();

// A guest call for the length of a scope.
struct GuestCallScope {
    explicit GuestCallScope(GuestCall kind) { clockwork_guest_call_begin(kind); }
    ~GuestCallScope() { clockwork_guest_call_end(); }
    GuestCallScope(const GuestCallScope&) = delete;
    GuestCallScope& operator=(const GuestCallScope&) = delete;
};

// The census. Bytes held is what the allocator says it gave; the counts are
// calls to malloc, calloc or realloc.
size_t   clockwork_guest_malloc_bytes();          // held now
uint32_t clockwork_guest_mallocs_boot();          // calls inside the boot entry points
uint32_t clockwork_guest_mallocs_processing();    // calls inside the processing entry points
uint32_t clockwork_guest_frees();                 // free calls inside any entry point

// Forget the call counts: a cold swap builds a new instance, and its calls
// start from nothing. The bytes held are not forgotten: what the old
// instance took and never freed is still the guest's, and still counted.
void clockwork_guest_malloc_reset();

// THE WHOLE PROGRAM'S MALLOCS AFTER BOOT, any scope: what reaches the
// system allocator once the engine is up, which the memory model says
// should be nothing (clockwork's own allocations have the rust heap, the
// guest's its bulk memory). The mark is taken when the engine is ready;
// the two readings count from it. Same wrap as the guest's census: 0 by
// absence where the link cannot wrap.
#ifdef __cplusplus
extern "C"
#endif
void     clockwork_malloc_mark_boot(void);          // "boot is done": count from here; exported to the worklet
// The host's own scratch through clockwork's doors (clockwork_worklet_alloc:
// the worklet's client handle, its poll array, its tap) is the host's, made
// once after the engine is ready, and not the engine's traffic: left out of
// the count for the length of a scope.
void     clockwork_malloc_host_scratch_begin();
void     clockwork_malloc_host_scratch_end();
uint32_t clockwork_mallocs_after_boot();            // malloc/calloc/realloc calls since the mark
size_t   clockwork_malloc_bytes_after_boot();       // bytes taken since the mark and still held (0 when net freed)

// Whether this build stands between the program and its allocator at all.
// False where the link could not wrap: the counts above are then 0 by
// absence, not by the guest's good behaviour.
bool clockwork_guest_malloc_counted();
