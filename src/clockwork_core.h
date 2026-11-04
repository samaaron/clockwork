// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_core.h — one engine's core: made and closed.
 *
 * A core is everything behind lanes.h for one engine, and the handle its
 * entry points take. A host makes one, gives it its geometry and memory with
 * clockwork_init, ticks it, and closes it. docs/ENGINES.md.
 *
 * A process holds as many cores as it makes, each independent of the
 * others; a wasm instance and a device hold one, and clockwork_core_new
 * while it is live answers
 * NULL with CLOCKWORK_E_PERM. A handle outlives its core: closed, every call
 * that takes it refuses it.
 *
 * Implemented in Rust (rust/clockwork-core). Pure C.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "clockwork_client.h"   /* ClockworkStatus */
#include "clockwork_ports.h"    /* ClockworkPorts */
#include "clockwork_event_sink.h"   /* ClockworkSinks */
#include "clockwork_heap.h"         /* ClockworkHeap */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkCore ClockworkCore;

/* Make a core. NULL with *status (when status is not NULL) set to
 * CLOCKWORK_E_PERM where a process holds one core and has it,
 * CLOCKWORK_E_NOMEM when it could not be made. */
ClockworkCore* clockwork_core_new(ClockworkStatus* status);

/* Close a core: it refuses new entries, wakes whoever waits on it, waits
 * until no thread is inside it, then frees it (docs/ENGINES.md). NULL,
 * or a core already closed or closing, is ignored: the first close is the
 * close, and others return at once. */
void clockwork_core_close(ClockworkCore* core);

/* Whether `core` was made and is not yet closed. */
bool clockwork_core_live(const ClockworkCore* core);

/* Whether `core` has begun to close: nothing of it may be read from here. */
bool clockwork_core_closing(const ClockworkCore* core);

/* ── The handle's words ──────────────────────────────────────────────────────
 * A handle is never freed, so what a thread must be able to touch without
 * knowing whether the core is still there lives on it: the gate, a count of
 * threads inside the core without the gate, and the word a client waits on.
 * A core is never freed under a thread that is using it: the close takes
 * the gate, then waits for the count to reach zero. */

/* Take `core`'s gate (core_state.h, GateHold): CLOCKWORK_GATE_TAKEN; or,
 * with `wait` false, CLOCKWORK_GATE_HELD when another thread holds it,
 * counted as a block that rendered nothing; CLOCKWORK_GATE_CLOSED once the
 * core is closing or gone, which a waiter is also told when the close begins
 * under it. Never from the thread that holds it: its entry says so. */
#define CLOCKWORK_GATE_TAKEN  0
#define CLOCKWORK_GATE_HELD   1
#define CLOCKWORK_GATE_CLOSED 2
int  clockwork_core_gate_take(const ClockworkCore* core, bool wait);
void clockwork_core_gate_release(const ClockworkCore* core);
bool clockwork_core_gate_held(const ClockworkCore* core);
/* Blocks that found the gate held and rendered nothing; threads waiting for
 * it. Diagnostics. */
uint32_t clockwork_core_gate_busy(const ClockworkCore* core);
uint32_t clockwork_core_gate_waiters(const ClockworkCore* core);

/* Count this thread inside `core` without its gate, for what it reads of the
 * core until clockwork_core_unhold; a close waits for it. False once the core
 * is closing, when nothing of it may be read. An entry without the gate
 * (clockwork_core_enter, below) holds the core this way until it is left. */
bool clockwork_core_hold(const ClockworkCore* core);
void clockwork_core_unhold(const ClockworkCore* core);

/* The word a client sleeps on until the engine says something
 * (clockwork_client_wait): bumped once per block that wrote RT-out and once
 * per NRT-out write, any thread; read; and waited on until it moves past
 * `old`, `timeout_ms` passes (UINT32_MAX: no deadline), or the core begins to
 * close — 1, 0 and -1. The wait holds the core, so the core is not freed
 * under it. */
void     clockwork_core_said_bump(const ClockworkCore* core);
uint32_t clockwork_core_said_value(const ClockworkCore* core);
uint32_t clockwork_core_said_waiters(const ClockworkCore* core);   /* diagnostic */
int      clockwork_core_said_wait(const ClockworkCore* core, uint32_t old, uint32_t timeout_ms);

/* `core`'s id: the process's cores are numbered from 1 in the order they
 * were made. 0 when `core` is not live. */
uint32_t clockwork_core_id(const ClockworkCore* core);

/* ── For clockwork's own C++ (src/core_state.h) ────────────────────────────── */

/* `core`'s tap slots, for the tap calls (rust/clockwork-taps), or NULL when
 * `core` is not live. Any thread. */
typedef struct ClockworkTaps ClockworkTaps;   /* opaque: clockwork_taps::Taps */
const ClockworkTaps* clockwork_core_taps(const ClockworkCore* core);

/* `core`'s ports (clockwork_ports.h), or NULL when `core` is not live. Any
 * thread. */
const ClockworkPorts* clockwork_core_ports(const ClockworkCore* core);

/* `core`'s event sinks (clockwork_event_sink.h), or NULL when `core` is not
 * live. Any thread. */
const ClockworkSinks* clockwork_core_sinks(const ClockworkCore* core);

/* Take a place for `core`'s guest instance, before it is built, among the
 * `allowed` one process may hold (DSP_INSTANCES_ANY, the most a count
 * holds, is any). False when other cores hold them all, or `core` is not
 * live. A core already holding one keeps it, through its instance's free
 * and rebuild, so its own rebuild is never refused; it gives the place back
 * only for an instance that was never built, and closing the core gives it
 * back. The core's control thread. */
bool clockwork_core_guest_take(const ClockworkCore* core, uint32_t allowed);
void clockwork_core_guest_give(const ClockworkCore* core);

/* `core`'s heap (clockwork_heap.h), or NULL when `core` is not live. Any
 * thread. */
const ClockworkHeap* clockwork_core_heap(const ClockworkCore* core);

/* `core`'s C++ state, or NULL before its first clockwork_init or when `core`
 * is not live. Any thread, the audio thread's included. */
void* clockwork_core_state(const ClockworkCore* core);

/* Hand `core` its C++ state and what frees it when the core closes. Once:
 * false when `core` is not live, `state` is NULL, or the core has one. */
bool clockwork_core_adopt_state(const ClockworkCore* core, void* state, void (*free_state)(void*));

/* Where `core`'s arena is, so a client opened over it finds its core. */
void clockwork_core_set_memory(const ClockworkCore* core, const void* base, size_t bytes);

/* The live core whose arena holds `at`, or NULL: how a client opened over
 * memory in this process finds its core. */
ClockworkCore* clockwork_core_of_memory(const void* at);

/* The thread's entry (docs/ENGINES.md): the core a call works inside,
 * what kind of call it is and whether it holds the core's gate, entered as
 * it starts and left as it returns, so nesting gives back the caller's. A
 * guest call or a door entered inside a call keeps its core and its gate.
 * It routes allocations and names a log line's core; nothing else reads it.
 * clockwork_core_enter returns the entry the thread had, for
 * clockwork_core_leave; a kind this list does not name is entered as
 * control. Only the gate's holder says it holds the gate (core_state.h). */
typedef enum ClockworkEntryKind {
    CLOCKWORK_ENTRY_CONTROL          = 0,   /* no block rendering: an init, a rebuild, a teardown, a close; and no core */
    CLOCKWORK_ENTRY_RENDER           = 1,   /* a block */
    CLOCKWORK_ENTRY_DOOR             = 2,   /* a sink or a port opened, closed or reshaped */
    CLOCKWORK_ENTRY_GUEST_BOOT       = 3,   /* dsp_describe, the memory queries, dsp_new, dsp_free */
    CLOCKWORK_ENTRY_GUEST_PROCESSING = 4    /* dsp_process, dsp_osc, dsp_asset */
} ClockworkEntryKind;

typedef struct ClockworkEntry {
    ClockworkCore*     core;
    ClockworkEntryKind kind;
    bool               gate;   /* the call holds the core's gate */
} ClockworkEntry;

ClockworkEntry clockwork_core_enter(ClockworkEntry entry);
void           clockwork_core_leave(ClockworkEntry previous);
ClockworkEntry clockwork_core_entry(void);
ClockworkCore* clockwork_core_current(void);

#ifdef __EMSCRIPTEN__
/* The core this wasm memory holds, or NULL: for a worklet made again over
 * the same memory, which comes back to that core rather than making
 * another. A wasm instance holds one core. */
ClockworkCore* clockwork_core_held(void);
#endif

#ifdef __cplusplus
}
#endif
