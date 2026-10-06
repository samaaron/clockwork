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

/* macOS is told this process does latency-critical work (NSProcessInfo's
 * activity API: NSActivityLatencyCritical, user-initiated, the system still
 * free to sleep when idle) until the token is released. Without it a
 * background app with no window, playing nothing the OS can hear, is one App
 * Nap may put to sleep and whose timers it may throttle. `why` is the reason
 * Activity Monitor shows. NULL where there is nothing to hold. */
void* clockwork_hold_latency_critical(const char* why);
void  clockwork_release_latency_critical(void* token);

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

/* The hold, for a scope: a main, for the process's life. */
struct ClockworkLatencyCritical {
    void* token;
    explicit ClockworkLatencyCritical(const char* why) : token(clockwork_hold_latency_critical(why)) {}
    ~ClockworkLatencyCritical() { clockwork_release_latency_critical(token); }
    ClockworkLatencyCritical(const ClockworkLatencyCritical&) = delete;
    ClockworkLatencyCritical& operator=(const ClockworkLatencyCritical&) = delete;
};
#endif

#endif  // CLOCKWORK_COCOA_EVENT_PUMP_H
