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

// The engine's one logging verb, on every target. Frames a /clockwork/debug
// OSC message onto the OUT ring; the host surfaces it on its debug channel
// (a client's log pane, a native host's stderr, a test fixture's capture).
// printf-compatible signature; no trailing newline.
//
// Before the ring exists (init_memory has not run) a line still goes
// somewhere: to the console on the web; on native it is held while an engine
// is booting and replayed onto the ring once the ring is up, so a boot's
// device-setup story reaches the same channel as everything after it, and
// otherwise (no engine booting, or a boot that never reached the ring) it
// goes to stderr. Native code never writes to stdio itself — see
// scripts/check-logging.sh.
int clockwork_log(const char* fmt, ...);

#ifndef __EMSCRIPTEN__
// Native boot bracket around the hold described above. ClockworkEngine::init
// calls hold() first and release() as soon as the ring is up (and again on
// every exit, where it is a no-op if already released). Nested holds are
// refused: the first release drains.
void clockwork_log_hold(void);
void clockwork_log_release(void);

// The Rust subsystems' lines join the same channel: rust/clockwork-log
// exports this, and the engine installs a sink at boot that forwards to
// clockwork_log. Present whenever the umbrella is linked (CLOCKWORK_RUST_LOG).
void clockwork_rust_log_install(void (*sink)(const char* line, uint32_t len));
#endif


#ifdef __cplusplus
}
#endif
