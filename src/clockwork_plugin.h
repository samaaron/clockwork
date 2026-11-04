// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_plugin.h — a plugin binary built on Clockwork (docs/ENGINES.md,
 * 3.7): a shared module a host loads, perhaps beside another plugin with a
 * Clockwork of its own. It exports its entry points and nothing else, so no
 * host's dynamic linker can bind another module's calls to this module's
 * Clockwork, or this module's to another's.
 *
 * The build makes everything else in the module hidden
 * (clockwork_plugin_module in CMakeLists.txt); an entry point says it is one
 * with CLOCKWORK_PLUGIN_ENTRY:
 *
 *   CLOCKWORK_PLUGIN_ENTRY const clap_plugin_entry_t clap_entry = { ... };
 *   CLOCKWORK_PLUGIN_ENTRY int my_plugin_entry(void) { ... }
 *
 * Pure C.
 */
#pragma once

#ifdef __cplusplus
#define CLOCKWORK_PLUGIN_C extern "C"
#else
#define CLOCKWORK_PLUGIN_C
#endif

#if defined(_WIN32)
#define CLOCKWORK_PLUGIN_ENTRY CLOCKWORK_PLUGIN_C __declspec(dllexport)
#else
#define CLOCKWORK_PLUGIN_ENTRY CLOCKWORK_PLUGIN_C __attribute__((visibility("default")))
#endif
