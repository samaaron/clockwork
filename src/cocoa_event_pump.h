// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * cocoa_event_pump.h — dispatch AppKit events from a main loop we already own.
 *
 * Only needed because this process can now own windows (a hosted plugin's
 * editor). Drawing works from the CFRunLoop pump alone; INPUT does not, because
 * AppKit delivers events through NSApplication's own dequeue-and-send cycle
 * rather than through a run loop source. See CocoaEventPump.mm.
 *
 * MAIN THREAD ONLY. No-op on every other platform, and a no-op on macOS until
 * something has actually created an NSApplication.
 */
#ifndef CLOCKWORK_COCOA_EVENT_PUMP_H
#define CLOCKWORK_COCOA_EVENT_PUMP_H

#ifdef __cplusplus
extern "C" {
#endif

void clockwork_pump_cocoa_events(void);

/* An autorelease pool for a main loop AppKit does not run.
 *
 * [NSApp run] drains a pool around every event; a loop of our own has none, so
 * everything AppKit autoreleases while we call it — the NSEvents the pump
 * dequeues, and every reference AppKit takes to a window while a plugin's
 * popup menu opens, tracks and closes — is never released. The visible symptom
 * was a plugin bridge owning hundreds of off-screen windows named "menu" (one
 * per popup ever opened in Surge XT's editor), all listed by OBS's window
 * capture. Push at the top of each loop turn, pop at the end. Nests. */
void* clockwork_autorelease_pool_push(void);
void  clockwork_autorelease_pool_pop(void* pool);

#ifdef __cplusplus
}

/* Scope guard for the C++ callers: one per loop turn. */
struct ClockworkAutoreleasePool {
    void* pool;
    ClockworkAutoreleasePool() : pool(clockwork_autorelease_pool_push()) {}
    ~ClockworkAutoreleasePool() { clockwork_autorelease_pool_pop(pool); }
    ClockworkAutoreleasePool(const ClockworkAutoreleasePool&) = delete;
    ClockworkAutoreleasePool& operator=(const ClockworkAutoreleasePool&) = delete;
};
#endif

#endif  // CLOCKWORK_COCOA_EVENT_PUMP_H
