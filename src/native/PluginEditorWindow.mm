// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * PluginEditorWindow.mm — the plugin's own editor, in a window owned by the
 * ENGINE.
 *
 * WHY THIS EXISTS AT ALL. A native view can only be parented into a window
 * belonging to the process that created it. The plugin that makes sound lives
 * here, in the audio engine; a GUI in another process therefore cannot show
 * that plugin's editor, and hosting a SECOND instance to draw with is the
 * compromise this file removes. Two instances means presets do not transfer,
 * meters read nothing, and every parameter has to be mirrored by hand — all of
 * which disappear once the editor belongs to the instance that is running.
 *
 * WHY IT CAN LIVE HERE. The engine is not as headless as it looks. It already
 * links AppKit (JUCE's CoreAudio backend brings it), and its main thread
 * already pumps a Cocoa run loop — the host calls CFRunLoopRunInMode in its
 * shutdown-wait loop, because AUHAL callbacks and GameController discovery both
 * need it. A window created here is therefore serviced, drawn and clicked like
 * any other, with no new thread and no new loop.
 *
 * MAIN THREAD ONLY, and not negotiable: every AppKit call below must run there,
 * and a VST3 view says the same of itself. The OSC command that opens an editor
 * arrives on the control thread, so it hops with dispatch_async — see
 * EngineControl.
 *
 * WHAT THIS DELIBERATELY IS NOT. It is not an embedding of the editor inside
 * another application's window; that needs a cross-process view bridge which
 * macOS offers no public API for. The window floats, which is what a DAW does
 * with a plugin editor anyway, and it carries a real benefit: a plugin that
 * crashes takes this process down and not the editor that was driving it.
 */
#import <Cocoa/Cocoa.h>

#include "plugin_editor_window.h"
#include "plugin_host.h"

#include <cstdio>
#include <map>
#include <string>

// ── One window per plugin ────────────────────────────────────────────────────

@interface ClockworkEditorWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) struct HostedPlugin* plugin;
@end

@implementation ClockworkEditorWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    // Detach BEFORE the window and its content view go away. A plugin whose
    // view outlives its parent is the standard way to crash a host, and here
    // that would take the audio engine with it.
    if (_plugin) plugin_editor_close(_plugin);
    [sender orderOut:nil];
    return NO;   // hide rather than destroy, so reopening is instant
}
@end

namespace {

struct Entry {
    NSWindow*                 window = nil;
    ClockworkEditorWindowDelegate*  delegate = nil;
};

std::map<struct HostedPlugin*, Entry>& registry() {
    static std::map<struct HostedPlugin*, Entry> r;
    return r;
}

/* NSApp has to exist before any window will draw, and a bare Mach-O executable
   — which this is — does not get one for free. Accessory policy: the engine can
   show windows and take key focus, without claiming a Dock tile as though it
   were an application the user launched. Idempotent; safe to call every time. */
void ensureApp() {
    static bool done = false;
    if (done) return;
    done = true;
    [NSApplication sharedApplication];
    if ([NSApp activationPolicy] == NSApplicationActivationPolicyProhibited)
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    // Without this NSApp is constructed but not STARTED: its event machinery,
    // main menu and window ordering are only set up by -finishLaunching, which
    // [NSApp run] would normally call. This process never calls run — it pumps
    // events itself (CocoaEventPump.mm) — so it has to do this part too, or
    // windows appear but behave oddly and key handling is unreliable.
    [NSApp finishLaunching];
}

}  // namespace

extern "C" {

void clockwork_editor_window_register_app(void) {
    if (![NSThread isMainThread]) return;
    ensureApp();
}

int clockwork_editor_window_open(struct HostedPlugin* p, const char* title) {
    if (!p) return 0;
    if (![NSThread isMainThread]) {
        fprintf(stderr, "[plugin-editor] refused: not the main thread\n");
        return 0;
    }
    fprintf(stderr, "[plugin-editor] open '%s'\n", title ? title : "?");
    ensureApp();

    Entry& e = registry()[p];
    if (e.window) {                            // already built: just show it
        if (!plugin_editor_open(p, (__bridge void*)[e.window contentView])) {
            // Reattaching can fail if the plugin tore its view down; the window
            // is still valid, so say so rather than leaving an empty frame up.
            return 0;
        }
        [e.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        return 1;
    }

    if (!plugin_editor_has(p)) {
        fprintf(stderr, "[plugin-editor] plugin reports no editor\n");
        return 0;
    }

    // Opened at a nominal size and corrected below once the view reports what
    // it actually wants — asking before attaching gives no useful answer.
    NSRect frame = NSMakeRect(0, 0, 800, 500);
    NSWindow* w = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                             | NSWindowStyleMaskMiniaturizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [w setTitle:[NSString stringWithUTF8String:(title ? title : "Plugin")]];
    [w setReleasedWhenClosed:NO];
    [w center];

    if (!plugin_editor_open(p, (__bridge void*)[w contentView])) {
        fprintf(stderr, "[plugin-editor] view refused to attach\n");
        return 0;   // ARC releases the window
    }

    uint32_t vw = 0, vh = 0;
    if (plugin_editor_size(p, &vw, &vh) && vw > 0 && vh > 0) {
        // setContentSize, not setFrame: the numbers the plugin gives are its
        // VIEW's, and the title bar is not part of them.
        [w setContentSize:NSMakeSize((CGFloat)vw, (CGFloat)vh)];
        [w center];
    }

    ClockworkEditorWindowDelegate* d = [[ClockworkEditorWindowDelegate alloc] init];
    d.plugin = p;
    [w setDelegate:d];

    e.window = w;
    e.delegate = d;

    [w makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    fprintf(stderr, "[plugin-editor] window up (%ux%u), policy=%ld\n",
            vw, vh, (long)[NSApp activationPolicy]);
    return 1;
}

void clockwork_editor_window_close(struct HostedPlugin* p) {
    if (!p || ![NSThread isMainThread]) return;
    auto it = registry().find(p);
    if (it == registry().end()) return;
    plugin_editor_close(p);
    [it->second.window orderOut:nil];
}

/* Called when the plugin itself is going away, so the window must not outlive
   it holding a dangling pointer. */
void clockwork_editor_window_forget(struct HostedPlugin* p) {
    if (!p) return;
    auto it = registry().find(p);
    if (it == registry().end()) return;
    if ([NSThread isMainThread]) {
        it->second.delegate.plugin = nullptr;
        [it->second.window setDelegate:nil];
        [it->second.window orderOut:nil];
    }
    registry().erase(it);
}

/* Marshal onto the main thread and return at once. The engine's main loop
   pumps CFRunLoop every 100ms, so the window appears within that. */
void clockwork_editor_window_show_async(struct HostedPlugin* p, const char* title) {
    if (!p) return;
    NSString* t = [NSString stringWithUTF8String:(title ? title : "Plugin")];
    dispatch_async(dispatch_get_main_queue(), ^{
        clockwork_editor_window_open(p, [t UTF8String]);
    });
}

void clockwork_editor_window_hide_async(struct HostedPlugin* p) {
    if (!p) return;
    dispatch_async(dispatch_get_main_queue(), ^{ clockwork_editor_window_close(p); });
}

int clockwork_editor_window_is_open(struct HostedPlugin* p) {
    if (!p) return 0;
    auto it = registry().find(p);
    return (it != registry().end() && it->second.window && [it->second.window isVisible]) ? 1 : 0;
}

}  // extern "C"
