// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * DsoTestUtils.h — dlopen/dlsym/dlclose for the plugin tests, on every platform.
 *
 * The plugin cases that reach past plugin_host.h load the test plugin's
 * module themselves, to call a test boundary it exports (ClockworkTestGainTurn,
 * ClockworkTestGainLastContext, clockwork_clap_gain_last_transport). On Windows that is
 * LoadLibrary rather than dlopen; a shim under dlfcn's names keeps those
 * cases reading as the one thing they are everywhere. The path is UTF-8 (it
 * came from CMake), so it goes through the wide entry point.
 */
#pragma once

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX 1
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#  include "clockwork_path.h"
#  define RTLD_NOW   0
#  define RTLD_LOCAL 0
static inline void* dlopen(const char* path, int) {
    return LoadLibraryExW(clockwork_path::to_wide(path ? path : "").c_str(), nullptr,
                          LOAD_WITH_ALTERED_SEARCH_PATH);
}
static inline void* dlsym(void* h, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), name));
}
static inline int dlclose(void* h) { return FreeLibrary(static_cast<HMODULE>(h)) ? 0 : -1; }
#else
#  include <dlfcn.h>
#endif
