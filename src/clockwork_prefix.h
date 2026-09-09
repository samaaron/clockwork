// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_prefix.h — the reserved address prefix, and nothing else.
 *
 * The rule it serves is stated in clockwork_sys.h: an address beginning with this
 * prefix is the Clockwork's; anything else is the DSP's. The string itself lives
 * here, alone, in a header with no dependencies, so that every part of
 * clockwork — the scheduler's wire parser, the debug-packet builder, the
 * subsystem boundaries, the route table — can derive its addresses and its byte
 * offsets from ONE definition without dragging in an OSC library.
 *
 * Change the prefix by changing the single #define below. Nothing else in the
 * C++ tree spells it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// ── THE reserved prefix, trailing slash included ────────────────────────────
#define CLOCKWORK_SYS_PREFIX_LIT "/clockwork/"

// CLOCKWORK_SYS("clock/tempo/get") == "/clockwork/clock/tempo/get", joined by the
// translation phase that concatenates adjacent string literals: no storage, no
// runtime cost, usable anywhere a string literal is (strcmp, BeginMessage, a
// static initialiser, a printf format).
#define CLOCKWORK_SYS(suffix) CLOCKWORK_SYS_PREFIX_LIT suffix

// Byte length of CLOCKWORK_SYS(suffix), NUL excluded. Use this instead of writing a
// count beside a memcmp: those counts are exactly what rots silently when the
// prefix changes length.
#define CLOCKWORK_SYS_LEN(suffix) (sizeof(CLOCKWORK_SYS(suffix)) - 1)

// Bytes an OSC address occupies on the wire: NUL included, padded to 4.
#define CLOCKWORK_SYS_PADDED(suffix) ((CLOCKWORK_SYS_LEN(suffix) + 4u) & ~3u)

inline constexpr char   CLOCKWORK_SYS_PREFIX[]    = CLOCKWORK_SYS_PREFIX_LIT;
inline constexpr size_t CLOCKWORK_SYS_PREFIX_LEN  = sizeof(CLOCKWORK_SYS_PREFIX) - 1;

// The rule, in one place. True iff this packet's address is clockwork's.
//
// BUNDLES ARE NEVER CLAIMED, and the reason is structural rather than a
// policy: a bundle begins "#bundle", not "/", so it has no single address to
// match on. It may contain many, addressed anywhere. Rather than open it and
// route each element — which would mean splitting a bundle whose whole purpose
// is to be applied atomically at one time — the entire bundle goes to the DSP,
// which is what upstream did too.
//
// A future timetag does NOT change that. Clockwork may HOLD the bundle (when
// the DSP does not hold its own schedule) and deliver it later, but it delivers
// it whole and unopened.
//
// THE CONSEQUENCE, which is easy to trip over: a /clockwork/ verb inside a bundle
// is NOT answered by clockwork. It is forwarded to the DSP with the rest of
// the bundle and dropped there. To schedule a clockwork verb, wrap it instead:
//
//     /clockwork/schedule <when> </clockwork/midi/...>
//
// which works because the scheduler's fire path re-applies this predicate to
// the payload's own address. Two scheduling mechanisms, routed differently on
// purpose.
inline bool clockwork_sys_claims(const uint8_t* osc, size_t len) noexcept {
    if (osc == nullptr || len < CLOCKWORK_SYS_PREFIX_LEN) return false;
    if (osc[0] != '/') return false;
    return std::memcmp(osc, CLOCKWORK_SYS_PREFIX, CLOCKWORK_SYS_PREFIX_LEN) == 0;
}
