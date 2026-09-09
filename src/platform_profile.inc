// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * platform_profile.inc — re-includable capability table (single source of truth).
 *
 * Input:  exactly one of CLOCKWORK_PROFILE_TARGET_DESKTOP / CLOCKWORK_PROFILE_TARGET_WASM / CLOCKWORK_PROFILE_TARGET_ESP32
 *         / CLOCKWORK_PROFILE_TARGET_TEENSY4 / CLOCKWORK_PROFILE_TARGET_FREESTANDING.
 * Output: CLOCKWORK_PROFILE_HOSTED_OS / CLOCKWORK_PROFILE_BYTE_ATOMICS / CLOCKWORK_PROFILE_TIERED_MEMORY / CLOCKWORK_PROFILE_HW_FLOAT64
 *         (each 0 or 1).
 *
 * Re-includable BY DESIGN (no include guard): the outputs are #undef'd at the top
 * so a host build can include this once per forced target and validate every
 * profile at compile time (see test/native/test_platform.cpp). platform.h
 * includes it once — target chosen from the real platform macros — and maps the
 * CLOCKWORK_PROFILE_* outputs to the public CLOCKWORK_HAS_* capabilities.
 *
 * To add a target: add one row here and a selection branch in platform.h.
 * To add a CAPABILITY: add it to the #undef block, give EVERY row a value, and
 * add it to the completeness guard at the bottom — which then fails the build
 * (via test_platform.cpp's per-target re-includes) for any row that forgot it,
 * rather than letting an undefined capability surface as a wrong value far away.
 */
#undef CLOCKWORK_PROFILE_HOSTED_OS
#undef CLOCKWORK_PROFILE_BYTE_ATOMICS
#undef CLOCKWORK_PROFILE_TIERED_MEMORY
#undef CLOCKWORK_PROFILE_HW_FLOAT64

#if defined(CLOCKWORK_PROFILE_TARGET_DESKTOP)
// Full desktop/native OS: boost::asio networking, cross-process shared memory,
// host-filesystem synthdef loading; std::atomic_flag; a single RAM tier;
// hardware double-precision FPU.
#    define CLOCKWORK_PROFILE_HOSTED_OS     1
#    define CLOCKWORK_PROFILE_BYTE_ATOMICS  1
#    define CLOCKWORK_PROFILE_TIERED_MEMORY 0
#    define CLOCKWORK_PROFILE_HW_FLOAT64    1
#elif defined(CLOCKWORK_PROFILE_TARGET_WASM)
// Browser / AudioWorklet: self-driven, OSC exchanged in-process (no sockets, no
// host FS); byte atomics are fine; a single heap (SAB pool / inline passthrough);
// the Wasm f64 type is hardware-fast.
#    define CLOCKWORK_PROFILE_HOSTED_OS     0
#    define CLOCKWORK_PROFILE_BYTE_ATOMICS  1
#    define CLOCKWORK_PROFILE_TIERED_MEMORY 0
#    define CLOCKWORK_PROFILE_HW_FLOAT64    1
#elif defined(CLOCKWORK_PROFILE_TARGET_ESP32)
// ESP32-S3: self-driven; Xtensa has 32-bit S32C1I but no byte test-and-set
// (std::atomic_flag won't link); fast internal SRAM + slow PSRAM => two-tier
// placement allocator. The LX7 FPU is single-precision only — every double op is
// software-emulated, so audio code uses float where double is just headroom.
#    define CLOCKWORK_PROFILE_HOSTED_OS     0
#    define CLOCKWORK_PROFILE_BYTE_ATOMICS  0
#    define CLOCKWORK_PROFILE_TIERED_MEMORY 1
#    define CLOCKWORK_PROFILE_HW_FLOAT64    0
#elif defined(CLOCKWORK_PROFILE_TARGET_TEENSY4)
// Teensy 4.x (NXP i.MX RT1062, Cortex-M7 @ 600 MHz): self-driven; ARMv7-M has
// byte exclusives (LDREXB/STREXB), so std::atomic_flag links; a single RAM tier
// (512 KiB DTCM/ITCM + 512 KiB OCRAM; PSRAM only if soldered to the 4.1 pads).
// The FPv5-D16 FPU is hardware double-precision, so the upstream double DSP runs
// at full speed (unlike the single-precision LX7).
#    define CLOCKWORK_PROFILE_HOSTED_OS     0
#    define CLOCKWORK_PROFILE_BYTE_ATOMICS  1
#    define CLOCKWORK_PROFILE_TIERED_MEMORY 0
#    define CLOCKWORK_PROFILE_HW_FLOAT64    1
#elif defined(CLOCKWORK_PROFILE_TARGET_FREESTANDING)
// Lean engine on a desktop CPU: the self-driven (hosted-OS-off) profile a
// browser/embedded host sees, but compiled and run natively. Exists so a normal
// CI runner can build + boot the shared engine through the lanes ABI with no
// JUCE, no Emscripten and no embedded toolchain — the "one C ABI for all build
// targets" guard. Capabilities INTENTIONALLY mirror WASM (lean, real byte
// atomics, single RAM tier, hardware f64) — it's "WASM's caps, native
// toolchain". Keep this row in sync with the CLOCKWORK_PROFILE_TARGET_WASM row above;
// test_platform.cpp pins both.
#    define CLOCKWORK_PROFILE_HOSTED_OS     0
#    define CLOCKWORK_PROFILE_BYTE_ATOMICS  1
#    define CLOCKWORK_PROFILE_TIERED_MEMORY 0
#    define CLOCKWORK_PROFILE_HW_FLOAT64    1
#else
#    error "platform_profile.inc: define exactly one CLOCKWORK_PROFILE_TARGET_*"
#endif

// Completeness guard: the selected row must define EVERY capability. Catches a
// new target row that forgets one, or a capability added to the table but not
// back-filled into an existing row, at compile time (test_platform.cpp re-includes
// this per target, so every row is validated). Keep this list == the #undef block.
#if !defined(CLOCKWORK_PROFILE_HOSTED_OS) || !defined(CLOCKWORK_PROFILE_BYTE_ATOMICS) || !defined(CLOCKWORK_PROFILE_TIERED_MEMORY)
#    error "platform_profile.inc: selected CLOCKWORK_PROFILE_TARGET_* is missing a capability define"
#endif
