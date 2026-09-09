// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * platform.h — capability profile for the active build target.
 *
 * The one place that turns raw platform macros (__EMSCRIPTEN__, ESP_PLATFORM, …)
 * into a small vocabulary of CAPABILITIES. Engine code keys off the capabilities,
 * never the platform macro — so adding a target is "add a profile row" (in
 * platform_profile.inc) plus a selection branch here, not "hunt #ifdefs".
 *
 * Public capabilities (each 1 or 0):
 *   CLOCKWORK_HAS_HOSTED_OS      full OS: sockets, host filesystem, cross-process shm
 *   CLOCKWORK_HAS_BYTE_ATOMICS   std::atomic_flag / __atomic_test_and_set usable
 *   CLOCKWORK_HAS_TIERED_MEMORY  distinct fast/bulk RAM regions (placement allocator)
 * Derived:
 *   CLOCKWORK_LEAN_TARGET        defined iff !CLOCKWORK_HAS_HOSTED_OS (self-driven build)
 *   CLOCKWORK_COLD_BSS           attribute placing large cold static tables in bulk RAM;
 *                         empty where there is a single RAM tier
 */
#pragma once

// ── Target selection ──────────────────────────────────────────────────────────
// CLOCKWORK_FREESTANDING is an explicit build opt-in, checked first so it overrides
// the desktop auto-detection: it compiles the lean/self-driven profile
// natively. Nothing in this repository defines it — no build or script passes
// it — so the branch is available and unused here. The rest auto-detect from
// the toolchain.
#if defined(CLOCKWORK_FREESTANDING)
#    define CLOCKWORK_PROFILE_TARGET_FREESTANDING 1
#elif defined(__EMSCRIPTEN__)
#    define CLOCKWORK_PROFILE_TARGET_WASM 1
#elif defined(ESP_PLATFORM)
#    define CLOCKWORK_PROFILE_TARGET_ESP32 1
#elif defined(__IMXRT1062__)
#    define CLOCKWORK_PROFILE_TARGET_TEENSY4 1
#else
#    define CLOCKWORK_PROFILE_TARGET_DESKTOP 1
#endif

// ── Capability values (single source of truth) ────────────────────────────────
#include "platform_profile.inc"

#define CLOCKWORK_HAS_HOSTED_OS      CLOCKWORK_PROFILE_HOSTED_OS
#define CLOCKWORK_HAS_BYTE_ATOMICS   CLOCKWORK_PROFILE_BYTE_ATOMICS
#define CLOCKWORK_HAS_TIERED_MEMORY  CLOCKWORK_PROFILE_TIERED_MEMORY
#define CLOCKWORK_HAS_HW_FLOAT64     CLOCKWORK_PROFILE_HW_FLOAT64

// ── Derived ───────────────────────────────────────────────────────────────────
// Self-driven targets (no hosted OS) build lean: no sockets, no cross-process
// shared memory, no loading definitions off a host filesystem. The engine's
// existing feature guards already key off CLOCKWORK_LEAN_TARGET.
#if !CLOCKWORK_HAS_HOSTED_OS
#    define CLOCKWORK_LEAN_TARGET 1
#endif

// clockwork_calc_t (DSP-recursion precision) + clockwork_calc_guard() live in a re-includable
// table so the native test can exercise both precisions on the host. Depends only
// on CLOCKWORK_HAS_HW_FLOAT64 (defined just above).
#include "calc_type.inc"

// Cold-BSS attribute: push large, rarely-touched static storage into bulk RAM
// so the audio path keeps fast RAM. In this tree it carries the whole ring
// arena (ring_buffer_storage) and the scheduler's data pool.
// Empty (no-op) on single-tier targets.
#if defined(CLOCKWORK_PROFILE_TARGET_ESP32)
#    include "esp_attr.h"
#    define CLOCKWORK_COLD_BSS EXT_RAM_BSS_ATTR
#else
#    define CLOCKWORK_COLD_BSS
#endif
