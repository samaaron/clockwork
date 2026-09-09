// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_discovery.h — where plugins live, and what is in them.
 *
 * plugin_host.h can describe a plugin you already have the path of. Nothing
 * could FIND one, so every consumer ended up with a string literal in it: this
 * repository's own panel shipped with "/Library/Audio/Plug-Ins/VST3/Surge
 * XT.vst3" compiled in, which is a demo of the loader rather than a plugin
 * host. This is the other half.
 *
 * NOT REALTIME-SAFE, and considerably worse than that: scanning OPENS each
 * plugin, which runs third-party initialisation code, touches the filesystem,
 * and takes as long as the slowest plugin installed. Control thread only, and
 * preferably not a thread anyone is waiting on.
 *
 * A SCAN CAN TAKE THE PROCESS DOWN. A plugin that crashes in its module entry
 * crashes whoever loaded it, and a plugin that hangs hangs them. That is not a
 * flaw in this code, it is the nature of the operation — every serious host
 * answers it by scanning in a SEPARATE PROCESS and keeping a cache, so a bad
 * plugin costs a scan rather than a session. This module does not do that yet;
 * it is deliberately callable from a process that can afford to die, which is
 * a GUI or a dedicated scanner, and NOT the process holding the audio device.
 */
#ifndef CLOCKWORK_PLUGIN_DISCOVERY_H
#define CLOCKWORK_PLUGIN_DISCOVERY_H

#include <stdint.h>
#include "plugin_host.h"   /* PluginFormat */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The platform's standard plugin folders, user-scope first.
 *
 * These are the locations the FORMATS define, not a preference: macOS puts
 * VST3 in Library/Audio/Plug-Ins/VST3 and CLAP in Library/Audio/Plug-Ins/CLAP,
 * Linux uses ~/.vst3 and /usr/lib/vst3. A host that invents its own location
 * finds nothing anyone has installed.
 *
 * User-scope first because a user's own build of a plugin should win over a
 * system copy of the same one.
 *
 * Returns how many paths exist, which may exceed `cap`. The strings are static
 * and outlive any call. Paths that do not exist on this machine are still
 * returned — whether a folder is present is the caller's to discover, and a
 * missing one simply scans to nothing.
 */
uint32_t plugin_search_paths(const char** out, uint32_t cap);

/*
 * Folders the USER adds, on top of the platform's. Discovery is not reliable
 * enough to be the only way in: a plugin installed to a vendor's own folder,
 * a developer's build tree, or a Windows install that ignored Common Files is
 * invisible to plugin_search_paths, and a host that offers no way to say
 * "look here too" has decided those plugins do not exist.
 *
 * Extra folders are scanned FIRST, so a copy in one of them wins over the
 * platform copy of the same plugin. Adding a folder twice is a no-op; a
 * folder that does not exist is kept (it may be a drive that is not mounted
 * yet) and scans to nothing. The list is process state; who persists it is
 * the caller's business (clockwork_track_rig_save writes it into a rig).
 *
 * Returns how many extra folders exist, which may exceed `cap`. The strings
 * are valid until the next add or remove.
 */
int      plugin_add_search_path(const char* dir);
int      plugin_remove_search_path(const char* dir);
uint32_t plugin_extra_search_paths(const char** out, uint32_t cap);

/*
 * Told the path of each plugin a scan is about to open, then NULL once it
 * is back. A scan runs foreign initialisation code; a host that may die in
 * there (the plugin bridge) records the path first so the crash can be
 * blamed. One listener; NULL clears it.
 */
typedef void (*PluginScanFn)(void* ctx, const char* path);
void plugin_set_scan_listener(PluginScanFn fn, void* ctx);

/*
 * One plugin found on disk.
 *
 * BY VALUE, unlike PluginDesc. PluginDesc borrows strings from the scanner's
 * own cache, which is fine for describing one file and useless for building a
 * list: the second scan invalidates the first one's pointers. A browser holds
 * hundreds of these for as long as the window is open, so they own their text.
 */
typedef struct PluginEntry {
    char         path[1024];   /* what to hand plugin_open */
    uint32_t     index;        /* which plugin inside that file */
    PluginFormat format;
    char         id[128];      /* the format's own stable identifier */
    char         name[128];
    char         vendor[128];

    /* Non-zero if the plugin generates. Copied from PluginDesc so a browser
       can place it in the chain without asking the user what kind of thing
       they just picked. */
    int32_t      is_instrument;
} PluginEntry;

/*
 * Describe every plugin in `dir`, one entry per plugin (a bundle may hold
 * several). Not recursive beyond the bundle: plugin folders are flat by
 * convention and descending into a plugin's own resources would open files
 * that are not plugins.
 *
 * Returns how many were found, which may exceed `cap`; writes at most `cap`.
 * A directory that does not exist returns 0, which is an answer rather than an
 * error — most machines have some of these folders and not others.
 *
 * Anything that fails to load is silently omitted. That is the same refusal
 * plugin_scan makes: a folder full of things that are not plugins is the
 * normal case, not a fault to report.
 */
uint32_t plugin_scan_dir(const char* dir, PluginEntry* out, uint32_t cap);

/*
 * Every plugin in every standard folder, de-duplicated by (id, format).
 *
 * De-duplication matters because the same plugin is routinely installed in
 * both the user and system folder, and a browser listing it twice makes the
 * user choose between two identical rows that differ only in a path they
 * cannot see. User scope wins, matching plugin_search_paths' order.
 *
 * Sorted by vendor then name, so the list is stable between runs — directory
 * order is not, and a browser whose contents move between launches cannot be
 * learned.
 */
uint32_t plugin_scan_all(PluginEntry* out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_PLUGIN_DISCOVERY_H */
