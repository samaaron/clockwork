// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_editor_window.h — show a hosted plugin's editor in a window this
 * process owns.
 *
 * plugin_host.h can create an editor and attach it to a native view somebody
 * else supplies. This goes one step further and supplies the window too, which
 * is the difference between an editor an embedder can host and an editor the
 * ENGINE can host.
 *
 * That difference matters more than it sounds. A native view can only be
 * parented into the process that made it, so a GUI in another process cannot
 * show the editor of the plugin that is actually making sound — it has to load
 * a second copy to draw with, and then presets, meters and every parameter
 * have to be mirrored by hand between two instances that only look alike. With
 * the window here, there is one instance.
 *
 * MACOS AND WINDOWS. Each relies on the main thread already pumping events:
 * a Cocoa run loop on macOS (the host's main pumps it, because AUHAL and
 * GameController need it anyway), the Win32 message pump the bridge turns on Windows
 * (PluginBridgeMain.cpp). The Windows half is written and not yet compiled
 * or run there. Linux compiles to no-ops that answer honestly rather than
 * pretending: a caller checks the return value.
 *
 * MAIN THREAD ONLY. Every one of these touches AppKit or a Win32 window that
 * belongs to one thread, and a VST3 view says the same of itself. Callers
 * arriving on the control thread must marshal — the _async entry points
 * below do exactly that.
 */
#ifndef CLOCKWORK_PLUGIN_EDITOR_WINDOW_H
#define CLOCKWORK_PLUGIN_EDITOR_WINDOW_H

#ifdef __cplusplus
extern "C" {
#endif

struct HostedPlugin;

/* Show this plugin's editor, building the window on first use and reusing it
   afterwards. Returns non-zero on success; 0 if the plugin has no editor, if
   the platform has no implementation, or if called off the main thread. */
int  clockwork_editor_window_open(struct HostedPlugin* p, const char* title);

/* Hide the window and detach the view. The window is kept, so reopening is
   immediate. */
void clockwork_editor_window_close(struct HostedPlugin* p);

/* Drop all record of this plugin. Call before plugin_close, or the window is
   left holding a pointer to a plugin that no longer exists. */
void clockwork_editor_window_forget(struct HostedPlugin* p);

int  clockwork_editor_window_is_open(struct HostedPlugin* p);

/* Become an application to the OS now, rather than when the first window
   opens. On macOS that is what puts this process in the lists that OBS's
   application capture and ScreenCaptureKit filters pick from, so a capture
   can be set up before any editor is shown and keeps working across editors
   opening and closing. Main thread. No-op elsewhere. */
void clockwork_editor_window_register_app(void);

/*
 * The same, callable from ANY thread.
 *
 * These marshal to the main thread themselves and return immediately, which is
 * what a caller on the OSC control thread needs. The marshalling lives on this
 * side so that callers stay plain C++ — pulling Cocoa into plugin_track.cpp
 * would make that whole translation unit Objective-C++ for one dispatch call.
 *
 * `title` is copied, so the caller's buffer need not outlive the call.
 */
void clockwork_editor_window_show_async(struct HostedPlugin* p, const char* title);
void clockwork_editor_window_hide_async(struct HostedPlugin* p);

#ifdef __cplusplus
}
#endif

#endif  // CLOCKWORK_PLUGIN_EDITOR_WINDOW_H
