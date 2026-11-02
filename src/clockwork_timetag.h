// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_timetag.h — an OSC timetag, as a type of its own.
 *
 * The value is NTP time in 32.32 fixed point, as OSC and NTP carry it (RFC
 * 5905 sec. 6): seconds since 1900 in the high 32 bits, the fraction of a
 * second in the low 32. JUCE's OSCTimeTag and oscpack hold the same 64 bits;
 * liblo's lo_timetag holds them as two 32-bit halves.
 *
 * A struct, not an integer, so that C and C++ will not compare or subtract it
 * as one. Read as a signed integer, every real time is negative and
 * "immediately" sorts after all of them. Read as an unsigned one, a time
 * before another is a difference near 2^64. The functions below get both
 * right.
 *
 * ORDER is serial-number arithmetic (RFC 1982): a comes before b when a - b,
 * wrapped to 64 bits and read as signed, is negative. That holds across the
 * NTP era rollover on 7 February 2036, for any two times within 68 years of
 * each other.
 *
 * IMMEDIATELY is OSC's 1 (63 zero bits, then a one), and Clockwork reads 0 the
 * same way. It names no moment: it comes before every real time and has no
 * offset from one.
 *
 * Passed and returned by value. A struct of one 64-bit field travels in a
 * register on every ABI Clockwork targets, wasm32 included, as a uint64_t
 * would.
 */
#ifndef CLOCKWORK_TIMETAG_H
#define CLOCKWORK_TIMETAG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkTimetag {
    uint64_t ntp;   /* NTP 32.32, the 64 bits OSC carries, in host byte order */
} ClockworkTimetag;

/* OSC's "immediately". */
static inline ClockworkTimetag clockwork_timetag_immediately(void) {
    ClockworkTimetag t = { 1u };
    return t;
}

/* Non-zero for OSC's "immediately" (1), and for 0, which Clockwork reads the
   same way. */
static inline int clockwork_timetag_is_immediate(ClockworkTimetag t) {
    return t.ntp <= 1u;
}

/* How far `to` is from `from`, in 32.32 seconds (2^32 to a second): positive
   when `to` is later. Both are real times, not immediately. */
static inline int64_t clockwork_timetag_offset(ClockworkTimetag from, ClockworkTimetag to) {
    return (int64_t)(to.ntp - from.ntp);
}

/* `t` moved by `offset` 32.32 seconds. */
static inline ClockworkTimetag clockwork_timetag_add(ClockworkTimetag t, int64_t offset) {
    t.ntp += (uint64_t)offset;
    return t;
}

/* Non-zero if `a` comes before `b`. Immediately comes before every real
   time. */
static inline int clockwork_timetag_before(ClockworkTimetag a, ClockworkTimetag b) {
    if (clockwork_timetag_is_immediate(a)) return !clockwork_timetag_is_immediate(b);
    if (clockwork_timetag_is_immediate(b)) return 0;
    return clockwork_timetag_offset(b, a) < 0;
}

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_TIMETAG_H */
