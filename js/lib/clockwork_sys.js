// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * clockwork_sys.js — the one address prefix clockwork reserves for itself.
 *
 * The split between clockwork and the DSP is mechanical: an address beginning
 * `/clockwork/` is Clockwork's and is answered by it; ANYTHING else is forwarded
 * to the DSP untouched. The client sits on the outside of that boundary, so every
 * clockwork address it sends or matches must be built here rather than written
 * out — the DSP's own verbs are the ones that stay opaque
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

/** The address of an encoded OSC packet: its first string. A bundle begins
 *  "#bundle" and that is what this returns for one, which no rule claims. */
export function oscAddress(bytes) {
  let end = 0;
  while (end < bytes.length && bytes[end] !== 0) end++;
  let s = "";
  for (let i = 0; i < end; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

/** isClockworkSys, for a packet still in bytes: clockwork_sys_claims in C.
 *  A bundle is never claimed (clockwork_prefix.h says why). */
export function isClockworkSysBytes(bytes) {
  if (!bytes || bytes.length < CLOCKWORK_SYS_PREFIX.length || bytes[0] !== 0x2f) return false;
  for (let i = 0; i < CLOCKWORK_SYS_PREFIX.length; i++)
    if (bytes[i] !== CLOCKWORK_SYS_PREFIX.charCodeAt(i)) return false;
  return true;
}

/** The reason a message for the guest is refused with when there is no guest
 *  to take it (CLOCKWORK_REFUSED_NO_INSTANCE in clockwork_sys.h). A client
 *  reads it as "wait for /clockwork/setup", never as an error to show. Every
 *  refusal has the one shape: /clockwork/error <address> <reason>
 *  <generation>, the generation the guest instance's at that moment
 *  (docs/RESTARTS.md). */
export const REASON_NO_INSTANCE = "no instance";
