// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_clock_math.cpp — pins clockwork contract for clock_math.h, the one
 * place the shared clock arithmetic is defined.
 *
 * clock_math.h exists so that the several unrelated translation units doing
 * this arithmetic cannot drift apart, which makes it exactly the header worth
 * pinning by value rather than by shape. Everything here is pure arithmetic:
 * no clock instance, no shared arena, no host process. (The single exception,
 * wallClockNTP(), is checked only against the system clock it wraps.)
 *
 * Contract pinned:
 *   - the epoch / unit constants, by value: kNtpEpochOffset,
 *     kNtpUnitsPerSecond, kMicrobeatsPerBeat, kDriftIirGain;
 *   - the tempo grid: beatAt / timeAtBeat / originFor are mutual inverses,
 *     and originFor places a chosen beat at a chosen time;
 *   - wrapPhase's non-negative wrap and its quantum <= 0 guard;
 *   - ntpToOscTimetag's 32.32 packing: seconds in the high word, fraction in
 *     the low word, truncating (never rounding up into the next second),
 *     exact at zero and across the whole 32-bit second range — including the
 *     range where the packed int64 is negative but the bit pattern is right;
 *   - round-tripping NTP seconds -> timetag -> NTP seconds;
 *   - the double <-> uint64 bit-cast pair (clockwork::doubleToBits /
 *     bitsToDouble) that the clock state and the test wall clock ride on.
 *
 * NOTE ON DIRECTION: clock_math.h declares only the NTP -> timetag direction.
 * There is no oscTimetagToNtp in the header, so the unpack side is spelled out
 * locally below (unpackTimetag) and the round-trip is pinned against it. If an
 * unpack helper is ever added to the header, this test should switch to it and
 * the local copy should go.
 *
 * NOTE ON SIGN: ntpToOscTimetag casts through uint32_t, so a negative
 * ntpSeconds is outside its domain (the cast would be undefined). Negative
 * inputs are therefore deliberately NOT pinned here — only zero and the
 * positive range are contract.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "clock/clock_math.h"
#include "shared_memory.h"   // clockwork::doubleToBits / bitsToDouble

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

using Catch::Approx;

namespace {

// The inverse of clockwork::ntpToOscTimetag. Local because the header only
// declares the forward direction — see NOTE ON DIRECTION above.
double unpackTimetag(int64_t tag) {
    const uint64_t u = static_cast<uint64_t>(tag);
    return static_cast<double>(u >> 32)
         + static_cast<double>(u & 0xFFFFFFFFull) / clockwork::kNtpUnitsPerSecond;
}

uint32_t timetagSeconds(int64_t tag) {
    return static_cast<uint32_t>(static_cast<uint64_t>(tag) >> 32);
}

uint32_t timetagFraction(int64_t tag) {
    return static_cast<uint32_t>(static_cast<uint64_t>(tag) & 0xFFFFFFFFull);
}

// One NTP fraction unit: the finest step a 32.32 timetag can express.
constexpr double kOneFractionUnit = 1.0 / 4294967296.0;

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Constants
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("clock_math: epoch and unit constants are exact", "[clock_math]") {
    // Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01):
    // 70 years, of which 17 were leap years.
    REQUIRE(clockwork::kNtpEpochOffset == 2208988800.0);
    REQUIRE(clockwork::kNtpEpochOffset == (70.0 * 365.0 + 17.0) * 86400.0);

    // An NTP timestamp is 64-bit fixed point, 32 bits of seconds over 32 bits
    // of fraction (RFC 5905 sec. 6), so one second is 2^32 units.
    REQUIRE(clockwork::kNtpUnitsPerSecond == 4294967296.0);
    REQUIRE(clockwork::kNtpUnitsPerSecond == std::pow(2.0, 32.0));

    // Beats travel as int64 microbeats.
    REQUIRE(clockwork::kMicrobeatsPerBeat == 1.0e6);

    // The drift IIRs converge ~1% per audio callback.
    REQUIRE(clockwork::kDriftIirGain == 0.01);
}

// ═════════════════════════════════════════════════════════════════════════════
// The tempo grid: beatAt / timeAtBeat / originFor
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("beatAt: the origin is beat zero", "[clock_math][beat]") {
    REQUIRE(clockwork::beatAt(1000.0, 1000.0, 120.0) == 0.0);
    REQUIRE(clockwork::beatAt(0.0, 0.0, 60.0) == 0.0);
}

TEST_CASE("beatAt: bpm sets beats per second", "[clock_math][beat]") {
    // 60 bpm = 1 beat/second, 120 bpm = 2 beats/second.
    REQUIRE(clockwork::beatAt(1010.0, 1000.0, 60.0)  == Approx(10.0));
    REQUIRE(clockwork::beatAt(1010.0, 1000.0, 120.0) == Approx(20.0));
    REQUIRE(clockwork::beatAt(1010.0, 1000.0, 30.0)  == Approx(5.0));
}

TEST_CASE("beatAt: times before the origin give negative beats",
          "[clock_math][beat]") {
    REQUIRE(clockwork::beatAt(999.0, 1000.0, 120.0) == Approx(-2.0));
}

TEST_CASE("timeAtBeat is the inverse of beatAt", "[clock_math][beat]") {
    const double origin = 3900000000.0;
    for (double bpm : {30.0, 60.0, 120.0, 128.5, 174.0}) {
        for (double beat : {-8.0, -0.25, 0.0, 1.0, 3.75, 4096.0}) {
            const double t = clockwork::timeAtBeat(beat, origin, bpm);
            REQUIRE(clockwork::beatAt(t, origin, bpm) == Approx(beat).margin(1e-9));
        }
    }
}

TEST_CASE("timeAtBeat: beat zero is the origin", "[clock_math][beat]") {
    REQUIRE(clockwork::timeAtBeat(0.0, 1234.5, 120.0) == 1234.5);
}

TEST_CASE("originFor places the chosen beat at the chosen time",
          "[clock_math][beat]") {
    // This is the whole point of originFor: feed its answer back into beatAt
    // at the same instant and get the beat you asked for.
    for (double bpm : {60.0, 120.0, 145.0}) {
        for (double beat : {-3.0, 0.0, 0.5, 17.25}) {
            const double t = 3900000123.75;
            const double origin = clockwork::originFor(beat, t, bpm);
            REQUIRE(clockwork::beatAt(t, origin, bpm) == Approx(beat).margin(1e-9));
        }
    }
}

TEST_CASE("originFor: beat zero leaves the origin at the given time",
          "[clock_math][beat]") {
    REQUIRE(clockwork::originFor(0.0, 555.25, 120.0) == 555.25);
}

// ═════════════════════════════════════════════════════════════════════════════
// wrapPhase
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("wrapPhase: within the quantum, the beat is its own phase",
          "[clock_math][phase]") {
    REQUIRE(clockwork::wrapPhase(0.0, 4.0)  == 0.0);
    REQUIRE(clockwork::wrapPhase(1.5, 4.0)  == Approx(1.5));
    REQUIRE(clockwork::wrapPhase(3.99, 4.0) == Approx(3.99));
}

TEST_CASE("wrapPhase: exact multiples of the quantum are phase zero",
          "[clock_math][phase]") {
    REQUIRE(clockwork::wrapPhase(4.0, 4.0)  == 0.0);
    REQUIRE(clockwork::wrapPhase(64.0, 4.0) == 0.0);
    REQUIRE(clockwork::wrapPhase(-8.0, 4.0) == 0.0);
}

TEST_CASE("wrapPhase: the result is never negative", "[clock_math][phase]") {
    // std::fmod alone returns -1 here; the whole reason wrapPhase exists is
    // that callers want a phase in [0, quantum).
    REQUIRE(clockwork::wrapPhase(-1.0, 4.0)  == Approx(3.0));
    REQUIRE(clockwork::wrapPhase(-5.5, 4.0)  == Approx(2.5));
    REQUIRE(clockwork::wrapPhase(-0.25, 4.0) == Approx(3.75));
}

TEST_CASE("wrapPhase: a non-positive quantum yields zero, not a division trap",
          "[clock_math][phase]") {
    REQUIRE(clockwork::wrapPhase(7.0, 0.0)  == 0.0);
    REQUIRE(clockwork::wrapPhase(7.0, -4.0) == 0.0);
    REQUIRE(clockwork::wrapPhase(-7.0, 0.0) == 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
// ntpToOscTimetag — NTP seconds -> 32.32 fixed point
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("ntpToOscTimetag: zero packs to zero", "[clock_math][timetag]") {
    REQUIRE(clockwork::ntpToOscTimetag(0.0) == 0);
}

TEST_CASE("ntpToOscTimetag: whole seconds land in the high word with a zero "
          "fraction", "[clock_math][timetag]") {
    const int64_t tag = clockwork::ntpToOscTimetag(1.0);
    REQUIRE(timetagSeconds(tag)  == 1u);
    REQUIRE(timetagFraction(tag) == 0u);
    REQUIRE(tag == (static_cast<int64_t>(1) << 32));

    const int64_t epoch = clockwork::ntpToOscTimetag(clockwork::kNtpEpochOffset);
    REQUIRE(timetagSeconds(epoch)  == 2208988800u);
    REQUIRE(timetagFraction(epoch) == 0u);
}

TEST_CASE("ntpToOscTimetag: the fraction is a binary fraction of 2^32",
          "[clock_math][timetag]") {
    REQUIRE(timetagFraction(clockwork::ntpToOscTimetag(3.5))    == 0x80000000u);
    REQUIRE(timetagFraction(clockwork::ntpToOscTimetag(3.25))   == 0x40000000u);
    REQUIRE(timetagFraction(clockwork::ntpToOscTimetag(3.75))   == 0xC0000000u);
    REQUIRE(timetagFraction(clockwork::ntpToOscTimetag(3.0625)) == 0x10000000u);
    // The seconds word is untouched by the fraction.
    REQUIRE(timetagSeconds(clockwork::ntpToOscTimetag(3.75)) == 3u);
}

TEST_CASE("ntpToOscTimetag: fractions truncate, never rounding into the next "
          "second", "[clock_math][timetag]") {
    // A value a hair under a whole second must keep the LOWER second. Rounding
    // up here would schedule a full unit early, once per second, forever.
    const double justUnder = 5.0 - kOneFractionUnit / 4.0;
    const int64_t tag = clockwork::ntpToOscTimetag(justUnder);
    REQUIRE(timetagSeconds(tag) == 4u);
    REQUIRE(timetagFraction(tag) == 0xFFFFFFFFu);
}

TEST_CASE("ntpToOscTimetag: sub-unit fractions floor to zero",
          "[clock_math][timetag]") {
    // Finer than 2^-32 s (~233 ps) is below the representable step.
    const int64_t tag = clockwork::ntpToOscTimetag(7.0 + kOneFractionUnit / 8.0);
    REQUIRE(timetagSeconds(tag)  == 7u);
    REQUIRE(timetagFraction(tag) == 0u);
}

TEST_CASE("ntpToOscTimetag: one fraction unit is representable",
          "[clock_math][timetag]") {
    const int64_t tag = clockwork::ntpToOscTimetag(7.0 + kOneFractionUnit);
    REQUIRE(timetagSeconds(tag)  == 7u);
    REQUIRE(timetagFraction(tag) == 1u);
}

TEST_CASE("ntpToOscTimetag: is monotonic over the second, as an unsigned "
          "quantity", "[clock_math][timetag]") {
    uint64_t prev = 0;
    for (int i = 0; i < 64; ++i) {
        const double ntp = 3900000000.0 + i * (1.0 / 64.0);
        const uint64_t u = static_cast<uint64_t>(clockwork::ntpToOscTimetag(ntp));
        if (i > 0) REQUIRE(u > prev);
        prev = u;
    }
}

TEST_CASE("ntpToOscTimetag: large seconds set the int64 sign bit but keep the "
          "bit pattern", "[clock_math][timetag]") {
    // NTP era 0 runs to 2^32 s (2036). Seconds >= 2^31 set the top bit of the
    // packed word, so the int64 reads negative while the 32.32 bit pattern is
    // still correct. Anything comparing timetags must do it unsigned.
    const double late = 4294967295.5;   // the last whole second of era 0
    const int64_t tag = clockwork::ntpToOscTimetag(late);
    REQUIRE(tag < 0);
    REQUIRE(timetagSeconds(tag)  == 4294967295u);
    REQUIRE(timetagFraction(tag) == 0x80000000u);
    REQUIRE(unpackTimetag(tag) == Approx(late).margin(kOneFractionUnit));

    // Below 2^31 seconds the packed word's top bit is clear, so the int64 is
    // positive. 3900000000 is NOT such a value — it is past 2^31 and reads
    // negative like every NTP time after 2038, which is the whole point of the
    // case above.
    REQUIRE(clockwork::ntpToOscTimetag(2000000000.0) > 0);
    REQUIRE(clockwork::ntpToOscTimetag(3900000000.0) < 0);
}

TEST_CASE("ntpToOscTimetag: round-trips NTP seconds through the timetag",
          "[clock_math][timetag]") {
    const double values[] = {
        0.0, 0.5, 1.0, 3.75,
        clockwork::kNtpEpochOffset,
        clockwork::kNtpEpochOffset + 0.125,
        3900000000.0, 3900000000.5, 3900000000.25,
        4294967295.0, 4294967295.75,
    };
    for (double v : values) {
        const int64_t tag = clockwork::ntpToOscTimetag(v);
        // Truncating, so the round-trip is never above the input and never
        // more than one fraction unit below it.
        const double back = unpackTimetag(tag);
        REQUIRE(back <= v);
        REQUIRE(back == Approx(v).margin(kOneFractionUnit));
    }
}

TEST_CASE("ntpToOscTimetag: a whole timetag equals seconds<<32 | fraction",
          "[clock_math][timetag]") {
    const int64_t tag = clockwork::ntpToOscTimetag(3900000000.5);
    const uint64_t expect = (static_cast<uint64_t>(3900000000u) << 32)
                          |  static_cast<uint64_t>(0x80000000u);
    REQUIRE(static_cast<uint64_t>(tag) == expect);
}

// ═════════════════════════════════════════════════════════════════════════════
// wallClockNTP
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("wallClockNTP: is the system clock plus the NTP epoch offset",
          "[clock_math]") {
    const double before = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const double ntp = wallClockNTP();
    const double after = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // One ULP of slack at the NTP magnitude, in both directions. wallClockNTP
    // adds the epoch offset, pushing the value to ~4e9 where a double's
    // resolution is about 5e-7 s; subtracting the offset back here rounds
    // again, so unixSeconds can land one ULP outside a bracket taken in the
    // Unix domain even when nothing is wrong. Anything larger than that is a
    // real ordering failure.
    const double unixSeconds = ntp - clockwork::kNtpEpochOffset;
    const double ulp = std::nextafter(ntp, ntp * 2.0) - ntp;
    REQUIRE(unixSeconds >= before - ulp);
    REQUIRE(unixSeconds <= after + ulp);

    // And it is genuinely in the NTP domain, not the Unix one: any plausible
    // "now" is well past the 2020 mark in NTP seconds.
    REQUIRE(ntp > 3786825600.0);   // 2020-01-01 as NTP seconds
}

// ═════════════════════════════════════════════════════════════════════════════
// doubleToBits / bitsToDouble
//
// These live in shared_memory.h rather than clock_math.h, but they are the
// carrier for every double the clock state and the test wall clock move
// through a 64-bit atomic, so the round-trip is pinned alongside the rest.
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("doubleToBits/bitsToDouble round-trip ordinary values",
          "[clock_math][bits]") {
    const double values[] = {
        0.0, 1.0, -1.0, 0.5, -0.5, 120.0,
        clockwork::kNtpEpochOffset, 3900000000.5, 1e-300, 1e300,
    };
    for (double v : values)
        REQUIRE(clockwork::bitsToDouble(clockwork::doubleToBits(v)) == v);
}

TEST_CASE("doubleToBits: known IEEE-754 bit patterns", "[clock_math][bits]") {
    REQUIRE(clockwork::doubleToBits(0.0)  == 0x0000000000000000ull);
    REQUIRE(clockwork::doubleToBits(1.0)  == 0x3FF0000000000000ull);
    REQUIRE(clockwork::doubleToBits(2.0)  == 0x4000000000000000ull);
    REQUIRE(clockwork::doubleToBits(-1.0) == 0xBFF0000000000000ull);
}

TEST_CASE("doubleToBits: negative zero is distinguishable from zero",
          "[clock_math][bits]") {
    // The zero bit pattern is the "unset" sentinel for the atomics that carry
    // these values (an unset test wall clock, an unset cached NTP), so -0.0
    // having its own non-zero pattern is worth knowing about.
    REQUIRE(clockwork::doubleToBits(0.0)  == 0ull);
    REQUIRE(clockwork::doubleToBits(-0.0) == 0x8000000000000000ull);
    REQUIRE(clockwork::bitsToDouble(0x8000000000000000ull) == 0.0);
    REQUIRE(std::signbit(clockwork::bitsToDouble(0x8000000000000000ull)));
}

TEST_CASE("doubleToBits/bitsToDouble round-trip infinities and NaN",
          "[clock_math][bits]") {
    const double inf = std::numeric_limits<double>::infinity();
    REQUIRE(clockwork::bitsToDouble(clockwork::doubleToBits(inf)) == inf);
    REQUIRE(clockwork::bitsToDouble(clockwork::doubleToBits(-inf)) == -inf);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(std::isnan(clockwork::bitsToDouble(clockwork::doubleToBits(nan))));
}

// ── The one reader everyone should use ───────────────────────────────────────
//
// The DSP consumes the session clock too, and whoever writes a DSP should not
// have to know that bpm is the release key anchoring beat_origin_ntp. These
// pin the reader that hides it.

TEST_CASE("readClockworkClock snapshots the session clock coherently", "[clock]") {
    ClockworkClockState st{};
    ClockworkClockState::initDefaults(st);

    SECTION("defaults are a usable clock, not zeros") {
        const auto s = readClockworkClock(&st);
        REQUIRE(s.bpm == Catch::Approx(clockwork::kDefaultBpm));
        REQUIRE_FALSE(s.is_playing);
    }

    SECTION("a null state answers with a default clock rather than dividing by zero") {
        const auto s = readClockworkClock(nullptr);
        REQUIRE(s.bpm == Catch::Approx(clockwork::kDefaultBpm));
        REQUIRE(std::isfinite(s.beatAt(1000.0)));
    }

    SECTION("beats follow tempo and origin together") {
        // 120 bpm = 2 beats/second, beat 0 at NTP 1000.
        st.bpm.store(clockwork::doubleToBits(120.0), std::memory_order_release);
        st.beat_origin_ntp.store(clockwork::doubleToBits(1000.0), std::memory_order_relaxed);
        const auto s = readClockworkClock(&st);
        REQUIRE(s.beatAt(1000.0) == Catch::Approx(0.0));
        REQUIRE(s.beatAt(1001.0) == Catch::Approx(2.0));
        REQUIRE(s.beatAt(1002.5) == Catch::Approx(5.0));
    }

    SECTION("transport carries its timestamp") {
        st.is_playing_at_ntp.store(clockwork::doubleToBits(2500.0), std::memory_order_relaxed);
        st.is_playing.store(1u, std::memory_order_release);
        const auto s = readClockworkClock(&st);
        REQUIRE(s.is_playing);
        REQUIRE(s.is_playing_at_ntp == Catch::Approx(2500.0));
    }

    SECTION("flags come through whole") {
        st.flags.store(SC_FLAG_LINK_ENABLED | SC_FLAG_START_STOP_SYNC,
                       std::memory_order_relaxed);
        const auto s = readClockworkClock(&st);
        REQUIRE((s.flags & SC_FLAG_LINK_ENABLED) != 0u);
        REQUIRE((s.flags & SC_FLAG_START_STOP_SYNC) != 0u);
    }
}
