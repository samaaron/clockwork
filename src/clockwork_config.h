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

#include <stdint.h>   // uint32_t in the declarations below: this header is included on its own (clockwork_product.h)

// ─── Which Clockwork ─────────────────────────────────────────────────────────
// Clockwork has no version number of its own. It is taken as a git submodule,
// pinned to a commit, so the commit is what says which Clockwork a build is: a
// number kept by hand beside it only drifts. The build passes the commit in,
// as git names it in this repository (CMakeLists.txt; a product's own build
// does the same): its short hash, with "-dirty" when the tree had changes.
// Without one it is "unknown".
//
// Compatibility is not this: CLOCKWORK_CLIENT_ABI_VERSION (clockwork_client.h),
// the feature bits and the asserted shared-memory layout say what a client may
// rely on.
#ifndef CLOCKWORK_COMMIT
#define CLOCKWORK_COMMIT "unknown"
#endif
// The commit's first eight hex digits as a number, where only a number fits
// (the metrics word, the client info's engine_version); 0 when unknown.
#ifndef CLOCKWORK_COMMIT_WORD
#define CLOCKWORK_COMMIT_WORD 0u
#endif

// Every compile-time size — the guest's memory defaults, the rings — sized
// per device.
#include "memory_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ClockworkCore;

// The engine's one logging verb, on every target: a line of `core`'s log.
// Frames a /clockwork/debug OSC message onto the core's OUT ring; the host
// surfaces it on its debug channel (a client's log pane, a native host's
// stderr, a test fixture's capture). printf-compatible; no trailing newline.
//
// Every line names its core: an engine's log carries its own lines and no
// other engine's (docs/ENGINES.md). Before the core's ring exists the
// line is held while the core boots (natively) and put on the ring once it
// is up, so a boot's story reaches the same channel as everything after it.
// A line with no core to carry it — `core` null, or a boot that never
// reached its ring — goes to the process's console (stderr; on the web the
// browser console), never to another engine. Native code never writes to
// stdio itself — see scripts/check-logging.sh.
int clockwork_log(struct ClockworkCore* core, const char* fmt, ...);

#ifndef __EMSCRIPTEN__
// Native boot bracket around `core`'s hold, described above.
// ClockworkEngine::init holds first; the build releases as soon as the ring
// is up, and init again on every way out, where it is a no-op if already
// released.
void clockwork_log_hold(struct ClockworkCore* core);
void clockwork_log_release(struct ClockworkCore* core);

// The Rust subsystems' lines join the same channel, each naming its core:
// rust/clockwork-log exports this, and clockwork_init installs a sink that
// forwards to clockwork_log. Present whenever the umbrella is linked
// (CLOCKWORK_RUST_LOG).
void clockwork_rust_log_install(void (*sink)(struct ClockworkCore* core, const char* line, uint32_t len));
#endif


#ifdef __cplusplus
}
#endif
