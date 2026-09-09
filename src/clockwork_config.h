// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_config.h — Shared definitions for all clockwork builds (WASM + native)
 *
 * Both the WASM AudioWorklet build and the native JUCE build share clockwork's
 * architecture: NRT mode, ring buffer communication, pre-allocated memory,
 * no malloc on the audio thread. This header provides common declarations
 * used across both platforms.
 */

#pragma once

// ─── Version ─────────────────────────────────────────────────────────────────
// Single source of truth for all builds (WASM, native exe, NIF).
// Bumped by hand — nothing in this repository rewrites it.
#define CLOCKWORK_VERSION_MAJOR 0
#define CLOCKWORK_VERSION_MINOR 71
#define CLOCKWORK_VERSION_PATCH 0

// String form for CLI / banners (e.g. "0.64.0")
#define CLOCKWORK_STRINGIFY2(x) #x
#define CLOCKWORK_STRINGIFY(x) CLOCKWORK_STRINGIFY2(x)
#define CLOCKWORK_VERSION_STRING \
    CLOCKWORK_STRINGIFY(CLOCKWORK_VERSION_MAJOR) "." \
    CLOCKWORK_STRINGIFY(CLOCKWORK_VERSION_MINOR) "." \
    CLOCKWORK_STRINGIFY(CLOCKWORK_VERSION_PATCH)

// Pre-allocated heap size for RT-safe allocations (used by clockwork_heap).
// Default 64MB for native builds (unused in WASM where emscripten manages
// memory); sized per device via memory_profile.h.
#include "memory_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

// Debug logging — frames a /clockwork/debug OSC message onto the OUT ring on
// both WASM and native; the host surfaces it on its debug channel. No-ops until
// memory is initialised. printf-compatible signature. (clockwork_log feeds the debug
// channel; native host logging to the terminal uses fprintf(stderr) directly.)
int clockwork_log(const char* fmt, ...);


#ifdef __cplusplus
}
#endif
