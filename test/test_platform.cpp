// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_platform.cpp — pins clockwork's capability platform layer:
 * src/platform.h, src/platform_profile.inc and src/calc_type.inc.
 *
 * platform.h folds the scattered raw-platform branches (ESP_PLATFORM,
 * __EMSCRIPTEN__, __IMXRT1062__, the freestanding opt-in) behind a small set of
 * CAPABILITY macros. Everything downstream keys off the capabilities, never the
 * raw platform macro — so adding a target is "add a profile row", not "hunt
 * #ifdefs".
 *
 * Two contracts are pinned here, and both are compile-time by construction:
 *
 *   1. The capability matrix. Capability values per target live in the
 *      re-includable platform_profile.inc (single source of truth). This host
 *      build validates EVERY target profile at compile time by re-including the
 *      table once per forced target — no cross-compiling. The active build's
 *      derived macros (CLOCKWORK_LEAN_TARGET, CLOCKWORK_COLD_BSS) are checked directly,
 *      since the host compiles the desktop branch.
 *
 *   2. clockwork_calc_t / clockwork_calc_guard (calc_type.inc). The host is desktop, so
 *      clockwork_calc_t is double and the guard is a pass-through; on its own the host
 *      never compiles the float branch. calc_type.inc is re-includable and pulls
 *      in no platform headers, so it is re-included below forced to single
 *      precision, in its own namespace, and the float-only behaviour — the
 *      divergence guard a single-precision target depends on — is tested here.
 */
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

struct Caps {
    int hosted_os;
    int byte_atomics;
    int tiered_memory;
    int hw_float64;
};

// ── Matrix: probe each target profile via the re-includable capability table ──
// Input to the table is exactly one CLOCKWORK_PROFILE_TARGET_*; output is the CLOCKWORK_PROFILE_* values.
// We capture each into a constexpr so the matrix is asserted at compile time.

#undef CLOCKWORK_PROFILE_TARGET_DESKTOP
#undef CLOCKWORK_PROFILE_TARGET_WASM
#undef CLOCKWORK_PROFILE_TARGET_ESP32
#undef CLOCKWORK_PROFILE_TARGET_TEENSY4
#undef CLOCKWORK_PROFILE_TARGET_FREESTANDING

#define CLOCKWORK_PROFILE_TARGET_DESKTOP 1
#include "platform_profile.inc"
constexpr Caps kDesktop{ CLOCKWORK_PROFILE_HOSTED_OS, CLOCKWORK_PROFILE_BYTE_ATOMICS, CLOCKWORK_PROFILE_TIERED_MEMORY, CLOCKWORK_PROFILE_HW_FLOAT64 };
#undef CLOCKWORK_PROFILE_TARGET_DESKTOP

#define CLOCKWORK_PROFILE_TARGET_WASM 1
#include "platform_profile.inc"
constexpr Caps kWasm{ CLOCKWORK_PROFILE_HOSTED_OS, CLOCKWORK_PROFILE_BYTE_ATOMICS, CLOCKWORK_PROFILE_TIERED_MEMORY, CLOCKWORK_PROFILE_HW_FLOAT64 };
#undef CLOCKWORK_PROFILE_TARGET_WASM

#define CLOCKWORK_PROFILE_TARGET_ESP32 1
#include "platform_profile.inc"
constexpr Caps kEsp32{ CLOCKWORK_PROFILE_HOSTED_OS, CLOCKWORK_PROFILE_BYTE_ATOMICS, CLOCKWORK_PROFILE_TIERED_MEMORY, CLOCKWORK_PROFILE_HW_FLOAT64 };
#undef CLOCKWORK_PROFILE_TARGET_ESP32

#define CLOCKWORK_PROFILE_TARGET_TEENSY4 1
#include "platform_profile.inc"
constexpr Caps kTeensy4{ CLOCKWORK_PROFILE_HOSTED_OS, CLOCKWORK_PROFILE_BYTE_ATOMICS, CLOCKWORK_PROFILE_TIERED_MEMORY, CLOCKWORK_PROFILE_HW_FLOAT64 };
#undef CLOCKWORK_PROFILE_TARGET_TEENSY4

#define CLOCKWORK_PROFILE_TARGET_FREESTANDING 1
#include "platform_profile.inc"
constexpr Caps kFreestanding{ CLOCKWORK_PROFILE_HOSTED_OS, CLOCKWORK_PROFILE_BYTE_ATOMICS, CLOCKWORK_PROFILE_TIERED_MEMORY, CLOCKWORK_PROFILE_HW_FLOAT64 };
#undef CLOCKWORK_PROFILE_TARGET_FREESTANDING

// Desktop: full OS, byte atomics, single-tier RAM.
static_assert(kDesktop.hosted_os == 1, "desktop has hosted OS");
static_assert(kDesktop.byte_atomics  == 1, "desktop has byte atomics");
static_assert(kDesktop.tiered_memory == 0, "desktop is single-tier");
// WASM: lean (no hosted OS), byte atomics OK, single-tier.
static_assert(kWasm.hosted_os == 0, "wasm is lean (no hosted OS)");
static_assert(kWasm.byte_atomics  == 1, "wasm has byte atomics");
static_assert(kWasm.tiered_memory == 0, "wasm is single-tier");
// ESP32: lean, NO byte atomics (Xtensa), two-tier SRAM/PSRAM.
static_assert(kEsp32.hosted_os == 0, "esp32 is lean");
static_assert(kEsp32.byte_atomics  == 0, "esp32 lacks byte atomics");
static_assert(kEsp32.tiered_memory == 1, "esp32 is two-tier");
// Teensy 4.x: lean, byte atomics (ARMv7-M byte exclusives), single-tier (no
// PSRAM unless soldered to the 4.1 pads), hardware f64 (RT1062 FPv5-D16 FPU).
static_assert(kTeensy4.hosted_os == 0, "teensy4 is lean");
static_assert(kTeensy4.byte_atomics  == 1, "teensy4 has byte atomics");
static_assert(kTeensy4.tiered_memory == 0, "teensy4 is single-tier");
static_assert(kTeensy4.hw_float64 == 1, "teensy4 FPv5-D16 has hardware f64");
// Freestanding: the lean profile compiled natively (the CI build guard) —
// mirrors WASM: lean, byte atomics OK, single-tier, hardware f64.
static_assert(kFreestanding.hosted_os == 0, "freestanding is lean");
static_assert(kFreestanding.byte_atomics  == 1, "freestanding has byte atomics");
static_assert(kFreestanding.tiered_memory == 0, "freestanding is single-tier");
static_assert(kFreestanding.hw_float64 == 1, "freestanding is native: hardware f64");
// The freestanding row exists to be WASM's capabilities on a native toolchain,
// so the two rows must stay identical — a drift here silently makes the CI
// build guard stop guarding the profile it claims to.
static_assert(kFreestanding.hosted_os      == kWasm.hosted_os      &&
              kFreestanding.byte_atomics   == kWasm.byte_atomics   &&
              kFreestanding.tiered_memory  == kWasm.tiered_memory  &&
              kFreestanding.hw_float64     == kWasm.hw_float64,
              "freestanding must mirror the wasm capability row");
// Hardware double FPU: desktop + WASM yes (clockwork_calc_t == double); the ESP32 LX7
// no (clockwork_calc_t degrades to float). Freestanding mirrors WASM (native CPU).
static_assert(kDesktop.hw_float64 == 1, "desktop has a hardware double FPU");
static_assert(kWasm.hw_float64 == 1, "wasm f64 is hardware-fast");
static_assert(kEsp32.hw_float64 == 0, "esp32 LX7 FPU is single-precision");

// ── Active build: platform.h as compiled for the host (= desktop) ─────────────
#include "platform.h"

static_assert(CLOCKWORK_HAS_HOSTED_OS == 1, "host build is desktop: has hosted OS");
static_assert(CLOCKWORK_HAS_BYTE_ATOMICS  == 1, "host build has byte atomics");
static_assert(CLOCKWORK_HAS_TIERED_MEMORY == 0, "host build is single-tier");

// Derived: a desktop host is NOT a lean target.
#ifdef CLOCKWORK_LEAN_TARGET
#    error "desktop host must not define CLOCKWORK_LEAN_TARGET"
#endif

// Invariant: CLOCKWORK_LEAN_TARGET is exactly !CLOCKWORK_HAS_HOSTED_OS.
#if CLOCKWORK_HAS_HOSTED_OS && defined(CLOCKWORK_LEAN_TARGET)
#    error "hosted OS present but CLOCKWORK_LEAN_TARGET defined"
#endif
#if !CLOCKWORK_HAS_HOSTED_OS && !defined(CLOCKWORK_LEAN_TARGET)
#    error "no hosted OS but CLOCKWORK_LEAN_TARGET not derived"
#endif

// CLOCKWORK_COLD_BSS must always be defined (an empty attribute off-embedded) so the
// cold table declarations compile on every target.
#ifndef CLOCKWORK_COLD_BSS
#    error "CLOCKWORK_COLD_BSS must always be defined"
#endif
CLOCKWORK_COLD_BSS static int clockwork_cold_bss_probe = 0;  // empty expansion must compile on host

TEST_CASE("platform.h exposes a coherent capability matrix", "[platform]") {
    // Runtime mirror of the compile-time matrix (so the case is visible in the
    // suite and the values are double-checked at runtime too).
    CHECK(kDesktop.hosted_os == 1);
    CHECK(kDesktop.byte_atomics == 1);
    CHECK(kDesktop.tiered_memory == 0);
    CHECK(kWasm.hosted_os == 0);
    CHECK(kEsp32.byte_atomics == 0);
    CHECK(kEsp32.tiered_memory == 1);
    CHECK(kTeensy4.hosted_os == 0);
    CHECK(kTeensy4.byte_atomics == 1);
    CHECK(kTeensy4.tiered_memory == 0);
    CHECK(kTeensy4.hw_float64 == 1);
    CHECK(kFreestanding.hosted_os == 0);
    CHECK(kFreestanding.byte_atomics == 1);
    CHECK(kFreestanding.hw_float64 == 1);
    CHECK(kDesktop.hw_float64 == 1);
    CHECK(kEsp32.hw_float64 == 0);
    (void)clockwork_cold_bss_probe;
}

TEST_CASE("platform.h: every target is lean except the desktop host", "[platform]") {
    // "Lean" is not a fourth capability — it is derived from the absence of a
    // hosted OS, which is what makes adding a target a table row.
    CHECK(kDesktop.hosted_os == 1);
    CHECK(kWasm.hosted_os == 0);
    CHECK(kEsp32.hosted_os == 0);
    CHECK(kTeensy4.hosted_os == 0);
    CHECK(kFreestanding.hosted_os == 0);
}

// ── clockwork_calc_t / clockwork_calc_guard ──────────────────────────────────────────────

// The host (desktop) build must be full double precision with the guard a pure
// pass-through, i.e. hosted behaviour is unchanged by the clockwork_calc_t conversion.
static_assert(CLOCKWORK_HAS_HW_FLOAT64 == 1, "host build has a hardware double FPU");
static_assert(sizeof(clockwork_calc_t) == sizeof(double), "host clockwork_calc_t is double");

// Re-include the calc-type table forced to single precision, in its own
// namespace, so the float-only guard branch is compiled and testable on the
// host. CLOCKWORK_HAS_HW_FLOAT64 is restored to the host value (1) afterwards.
namespace flt {
#undef CLOCKWORK_HAS_HW_FLOAT64
#define CLOCKWORK_HAS_HW_FLOAT64 0
#include "calc_type.inc"
#undef CLOCKWORK_HAS_HW_FLOAT64
#define CLOCKWORK_HAS_HW_FLOAT64 1
} // namespace flt
static_assert(sizeof(flt::clockwork_calc_t) == sizeof(float), "forced-float clockwork_calc_t is float");
static_assert(CLOCKWORK_HAS_HW_FLOAT64 == 1, "CLOCKWORK_HAS_HW_FLOAT64 restored to the host value after the re-include");

TEST_CASE("clockwork_calc_guard: host double path is a pass-through", "[platform][calc]") {
    // double has the headroom, so nothing is clamped on the host.
    CHECK(clockwork_calc_guard(0.5) == 0.5);
    CHECK(clockwork_calc_guard(1.0e30) == 1.0e30);
    CHECK(std::isinf(clockwork_calc_guard(std::numeric_limits<double>::infinity())));
}

TEST_CASE("clockwork_calc_guard: float path resets non-finite / runaway state",
          "[platform][calc]") {
    using F = flt::clockwork_calc_t;
    // normal values pass through unchanged
    CHECK(flt::clockwork_calc_guard(F(0.5f)) == 0.5f);
    CHECK(flt::clockwork_calc_guard(F(-12345.0f)) == -12345.0f);
    // non-finite and finite-runaway reset to 0 (recover the recursion)
    CHECK(flt::clockwork_calc_guard(std::numeric_limits<F>::infinity()) == 0.0f);
    CHECK(flt::clockwork_calc_guard(-std::numeric_limits<F>::infinity()) == 0.0f);
    CHECK(flt::clockwork_calc_guard(std::numeric_limits<F>::quiet_NaN()) == 0.0f);
    CHECK(flt::clockwork_calc_guard(F(1.0e30f)) == 0.0f);
    // The bound is 1e15 — just inside it passes, just outside resets.
    CHECK(flt::clockwork_calc_guard(F(1.0e14f)) == F(1.0e14f));
    CHECK(flt::clockwork_calc_guard(F(1.0e16f)) == 0.0f);
    CHECK(flt::clockwork_calc_guard(F(-1.0e16f)) == 0.0f);
}

// A 2-pole recursion in the shape of a biquad's inner step, in float, with
// intentionally unstable coefficients (a pole well outside the unit circle).
// This is arithmetic, not audio: no signal chain is involved. Unguarded the
// float state diverges to non-finite; guarded it stays bounded.
TEST_CASE("clockwork_calc_guard bounds a diverging float recursion", "[platform][calc]") {
    using F = flt::clockwork_calc_t;
    const F b1 = F(2.5f), b2 = F(-1.0f); // |poles| > 1 -> unstable
    auto run = [&](bool guarded) {
        F y1 = 0, y2 = 0, peak = 0;
        for (int i = 0; i < 100000; ++i) {
            F in = (i == 0) ? F(1.0f) : F(0.0f); // unit impulse
            F y0 = in + b1 * y1 + b2 * y2;
            y2 = y1;
            y1 = y0;
            if (guarded) {
                y1 = flt::clockwork_calc_guard(y1);
                y2 = flt::clockwork_calc_guard(y2);
            }
            F a = std::fabs(y1);
            if (a > peak)
                peak = a;
        }
        return peak;
    };
    CHECK_FALSE(std::isfinite(run(false))); // unguarded: diverges
    F gpeak = run(true);
    CHECK(std::isfinite(gpeak)); // guarded: stays finite
    CHECK(gpeak < F(1.0e15f));   // and bounded below the guard threshold
}
