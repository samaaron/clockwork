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
// is to be applied atomically at one time — the entire bundle goes to the DSP.
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

// ── Which of clockwork's verbs the AUDIO THREAD performs ────────────────────
//
// The audio thread answers a verb inside its block when the verb is about the
// block: the liveness ping and echo; the ordering barrier (sync) and the
// clock snapshot, which must be answered where the hand-off to the guest
// happens; the schedule and its flush; an asset hand-off; an inbound event,
// which reaches the guest from there; and what plays a hosted track. Every
// other verb of clockwork's is CONTROL — notify, devices, the session tempo,
// a port's subsystems, a track's edits — and is the control pass's, which
// answers it with no audio block in the loop (docs/RESTARTS.md, "The control
// ring"). One rule, read at every boundary and by the engine's routes alike.
//
// `address` is the whole address, NUL-terminated, "/clockwork/..." included.
inline bool clockwork_sys_is_audio_verb(const char* address) noexcept {
    if (address == nullptr
        || std::strncmp(address, CLOCKWORK_SYS_PREFIX, CLOCKWORK_SYS_PREFIX_LEN) != 0)
        return false;
    const char* verb = address + CLOCKWORK_SYS_PREFIX_LEN;
    const auto is = [verb](const char* v) { return std::strcmp(verb, v) == 0; };
    const auto under = [verb](const char* p) { return std::strncmp(verb, p, std::strlen(p)) == 0; };
    // "midi/in/enable" opens a port: control, under a prefix whose other
    // verbs are the port's inbound events.
    return is("ping") || is("echo") || is("sync") || is("schedule") || is("sched/flush")
        || is("clock/state/get")
        || under("asset/")
        || (under("midi/in/") && !is("midi/in/enable")) || is("midi/ports")
        || under("gamepad/in/") || is("gamepad/devices")
        || is("track/note") || is("track/cc") || is("track/bend") || is("track/notes_off")
        || is("track/param") || is("track/plugin/param");
}

// ── Which of clockwork's audio-thread verbs hand the GUEST something ───────
//
// An asset, an inbound event, a scheduled message, the schedule's flush: each
// is written for the instance that is up, as much as the guest's own
// messages beside it, and is refused with them when that instance goes
// (docs/RESTARTS.md, rule 2) — at the door between instances, and below a
// teardown's mark. Clockwork's other verbs are answered with or without a
// guest. `address` is the whole address, NUL-terminated.
inline bool clockwork_sys_hands_to_guest(const char* address) noexcept {
    if (address == nullptr
        || std::strncmp(address, CLOCKWORK_SYS_PREFIX, CLOCKWORK_SYS_PREFIX_LEN) != 0)
        return false;
    const char* verb = address + CLOCKWORK_SYS_PREFIX_LEN;
    const auto is = [verb](const char* v) { return std::strcmp(verb, v) == 0; };
    const auto under = [verb](const char* p) { return std::strncmp(verb, p, std::strlen(p)) == 0; };
    return under("asset/")
        || (under("midi/in/") && !is("midi/in/enable"))
        || under("gamepad/in/")
        || is("schedule") || is("sched/flush");
}

// True iff this packet is clockwork's and answered with or without a guest:
// claimed by the prefix, its address whole within the packet, and handing
// the guest nothing. What the in ring takes between instances, and what the
// drain answers below a teardown's mark rather than refuse.
inline bool clockwork_sys_answers_without_guest(const uint8_t* osc, size_t len) noexcept {
    if (!clockwork_sys_claims(osc, len)) return false;
    size_t n = 0;
    while (n < len && osc[n] != '\0') ++n;
    if (n == len) return false;
    return !clockwork_sys_hands_to_guest(reinterpret_cast<const char*>(osc));
}

// True iff this packet is a CONTROL verb of clockwork's: claimed by the prefix
// (so never a bundle, never the guest's) and not the audio thread's. What the
// control ring takes, when the engine serves one.
inline bool clockwork_sys_control_takes(const uint8_t* osc, size_t len) noexcept {
    if (!clockwork_sys_claims(osc, len)) return false;
    // The address is NUL-terminated within the packet by OSC's framing; a
    // packet cut short of its NUL is not clockwork's to classify.
    size_t n = 0;
    while (n < len && osc[n] != '\0') ++n;
    if (n == len) return false;
    return !clockwork_sys_is_audio_verb(reinterpret_cast<const char*>(osc));
}
