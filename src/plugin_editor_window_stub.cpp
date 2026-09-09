// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_editor_window_stub.cpp — the Linux half of plugin_editor_window.h.
 *
 * Hosting a plugin editor inside the engine needs a window system and a run
 * loop the host already turns. macOS has both (the host's main pumps CFRunLoop
 * for AUHAL and GameController anyway; PluginEditorWindow.mm), and Windows has
 * the message pump the bridge runs (PluginEditorWindowWin.cpp). Linux would
 * need an X11/Wayland loop the engine does not run — so rather than guess at
 * one, this answers honestly and the caller falls back to whatever it does
 * when a plugin has no editor.
 */
#include "plugin_editor_window.h"

extern "C" {
int  clockwork_editor_window_open(struct HostedPlugin*, const char*) { return 0; }
void clockwork_editor_window_close(struct HostedPlugin*) {}
void clockwork_editor_window_forget(struct HostedPlugin*) {}
int  clockwork_editor_window_is_open(struct HostedPlugin*) { return 0; }
void clockwork_editor_window_register_app(void) {}
void clockwork_editor_window_show_async(struct HostedPlugin*, const char*) {}
void clockwork_editor_window_hide_async(struct HostedPlugin*) {}
}
