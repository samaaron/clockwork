// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * calc_type.inc — the clockwork_calc_t precision type + clockwork_calc_guard().
 *
 * Input:  CLOCKWORK_HAS_HW_FLOAT64 (0 or 1) must be defined before inclusion.
 * Output: `clockwork_calc_t` (double when the platform has a hardware double FPU, else
 *         float) and `clockwork_calc_guard()`.
 *
 * Deliberately re-includable (no include guard; each symbol lands in the current
 * scope): platform.h includes it once at file scope, and test/test_platform.cpp
 * re-includes it under two namespaces (one per forced CLOCKWORK_HAS_HW_FLOAT64) so the
 * float-only guard branch — which the host build otherwise never compiles — is
 * still tested on the host. It pulls in no platform headers for the same reason.
 *
 * Rationale: the conventional choice keeps per-sample recursions (IIR/biquad
 * filter state, accumulators) in double for numerical headroom. With a hardware double
 * FPU that is unchanged. On a single-precision FPU (Xtensa LX7) every double op
 * is software-emulated at roughly 10-30x the cost, so there the recursions run
 * in float, which has adequate headroom for filter state at audio rates.
 */
#if defined(__cplusplus)
#    if CLOCKWORK_HAS_HW_FLOAT64
using clockwork_calc_t = double;
#    else
using clockwork_calc_t = float;
#    endif

// clockwork_calc_guard — in float mode, defend a recursive IIR state against the
// runaway/NaN blow-up that double's headroom would otherwise absorb (resonant
// filters at high Q / low cutoff are the risk). A non-finite or runaway state
// resets to 0, recovering the filter the same way zapgremlins() does for
// NaN/denormals; unlike zapgremlins it also catches a still-finite runaway
// (e.g. 1e30). No-op when clockwork_calc_t is double. The 1e15 bound sits far above
// any legitimate audio-rate state and far below float's ~3.4e38 max.
static inline clockwork_calc_t clockwork_calc_guard(clockwork_calc_t x) {
#    if !CLOCKWORK_HAS_HW_FLOAT64
    if (!(x > -1.0e15f && x < 1.0e15f))
        return 0.0f; // NaN / Inf / runaway -> reset the recursion
#    endif
    return x;
}
#endif // __cplusplus
