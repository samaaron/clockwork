// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * clockwork_sys.js — the one address prefix clockwork reserves for itself.
 *
 * The split between clockwork and the DSP is mechanical: an address beginning
 * `/clockwork/` is Clockwork's and is answered by it; ANYTHING else is forwarded
 * to the DSP untouched. The client sits on the outside of that boundary, so every
 * clockwork address it sends or matches must be built here rather than written
 * out — the DSP's own verbs (`dspProfile`) are the ones that stay opaque
 * strings.
 *
 * THIS FILE IS THE ONLY PLACE THE STRING IS SPELLED in the JS tree. The C++ and
 * Rust trees each have their own single definition
 * (`CLOCKWORK_SYS_PREFIX_LIT` in `src/clockwork_prefix.h`; the `clockwork_sys!` macro in
 * `rust/clockwork-osc/src/lib.rs`) and all three must agree.
 */

/** The reserved prefix, trailing slash included. */
export const CLOCKWORK_SYS_PREFIX = "/clockwork/";

/** `clockworkSys("debug")` → `"/clockwork/debug"`. */
export function clockworkSys(suffix) {
  return CLOCKWORK_SYS_PREFIX + suffix;
}

/** True iff `address` is clockwork's. The trailing slash is the whole rule:
 *  "/clockwork-sys" and "/clockwork-system-status" are NOT claimed. */
export function isClockworkSys(address) {
  return typeof address === "string" && address.startsWith(CLOCKWORK_SYS_PREFIX);
}
