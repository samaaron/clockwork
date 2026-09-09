// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * CocoaEventPump.mm — deliver mouse and keyboard events to windows this
 * process owns.
 *
 * THE BUG THIS FIXES. A host's macOS loop calls CFRunLoopRunInMode, which
 * services run-loop sources: timers, CoreAudio's AUHAL callbacks,
 * GameController discovery, and the source the window server posts on. That is
 * enough for a window to APPEAR and to REDRAW — drawing happens through run
 * loop observers and CoreAnimation — so an editor hosted here looked perfectly
 * alive.
 *
 * It is not enough for input. AppKit does not dispatch events from a run loop
 * source; NSApplication dequeues them with -nextEventMatchingMask: and hands
 * each to -sendEvent:, and that is what [NSApp run] spends its life doing. With
 * no one draining the queue, every click and keystroke arrived, sat in NSApp's
 * event queue, and was never delivered. The window rendered, followed
 * parameter changes made elsewhere, and ignored the mouse entirely.
 *
 * WHY NOT JUST CALL [NSApp run]. Because it does not return. This process's
 * main thread has other work in that loop — the shutdown flag, the microphone
 * permission re-check — and an audio server is not an application whose life is
 * its event loop. Draining explicitly keeps the existing loop in charge and
 * adds event dispatch to it, which is the standard shape for embedding AppKit
 * in a main loop somebody else owns.
 *
 * SAFE WHEN THERE ARE NO WINDOWS. Until an editor is opened nothing calls
 * [NSApplication sharedApplication], so NSApp is nil, and messaging nil returns
 * nil — the drain loop exits immediately and costs a nil check per block of
 * idle time.
 */
#import <Cocoa/Cocoa.h>

#include "cocoa_event_pump.h"

extern "C" void clockwork_pump_cocoa_events(void) {
    if (NSApp == nil) return;          // no window has ever been opened

    // A bounded drain. Unbounded would let a flood of events (a fast drag, a
    // plugin animating under the cursor) starve the rest of the loop —
    // including the shutdown check — so this takes what is waiting and comes
    // back next turn for the rest.
    for (int i = 0; i < 64; ++i) {
        // A pool per event, as [NSApp run] keeps one: the event itself is
        // autoreleased, and so is much of what AppKit touches delivering it.
        // Without this every popup menu a plugin's editor opened stayed
        // allocated as an off-screen window for the life of the process.
        @autoreleasepool {
            NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:nil          // poll, never block
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
            if (e == nil) break;
            [NSApp sendEvent:e];
        }
    }
    // Lets AppKit run the deferred work it normally does between events:
    // window ordering, cursor rects, view invalidation.
    @autoreleasepool {
        [NSApp updateWindows];
    }
}

// The runtime's pool functions are not in a public header, but they are what
// @autoreleasepool compiles to and have been exported since 10.7.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void* pool);

extern "C" void* clockwork_autorelease_pool_push(void) { return objc_autoreleasePoolPush(); }
extern "C" void  clockwork_autorelease_pool_pop(void* pool) { objc_autoreleasePoolPop(pool); }
