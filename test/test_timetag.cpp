// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_timetag.cpp — clockwork::Timetag (src/clock/timetag.h) and the C
 * helpers in src/clockwork_timetag.h: immediately comes first, real times
 * order by serial-number arithmetic across the 2036 rollover, an offset is
 * signed, and nothing compares or subtracts a timetag as an integer.
 */
#include <catch2/catch_test_macros.hpp>

#include "clock/timetag.h"

#include <cmath>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <vector>

using clockwork::Timetag;

namespace {

constexpr uint64_t kSecond = 1ull << 32;

// 1 January 2026, 00:00 UTC, in NTP seconds.
constexpr double kNtp2026 = 3976214400.0;

// A timetag is not an integer, in either direction. Its arithmetic is a time
// point's: a time minus a time is a signed offset, a time plus an offset a
// time.
static_assert(!std::is_convertible_v<int64_t, Timetag>);
static_assert(!std::is_convertible_v<uint64_t, Timetag>);
static_assert(!std::is_constructible_v<Timetag, uint64_t>);
static_assert(!std::is_convertible_v<Timetag, int64_t>);
static_assert(!std::is_convertible_v<Timetag, uint64_t>);
static_assert(!std::totally_ordered_with<Timetag, int64_t>);
static_assert(std::is_same_v<decltype(Timetag() - Timetag()), int64_t>);
static_assert(std::is_same_v<decltype(Timetag() + int64_t{1}), Timetag>);
// And it is the C struct's size, so it passes the way a uint64_t does.
static_assert(sizeof(Timetag) == sizeof(uint64_t));
static_assert(sizeof(ClockworkTimetag) == sizeof(uint64_t));

}  // namespace

TEST_CASE("timetag: immediately comes before every real time, and 0 and 1 are both immediately",
          "[timetag]") {
    const Timetag now = Timetag::fromNtpSeconds(kNtp2026);
    // Read as int64_t, every real time is negative and 1 sorts after it.
    REQUIRE(static_cast<int64_t>(now.ntp()) < 1);

    CHECK(Timetag().isImmediate());
    CHECK(Timetag::immediately().isImmediate());
    CHECK(Timetag() == Timetag::immediately());
    CHECK_FALSE(Timetag::fromNtp(2).isImmediate());
    CHECK(Timetag::immediately() < now);
    CHECK(Timetag() < now);
    CHECK_FALSE(now < Timetag::immediately());
    CHECK(clockwork_timetag_before(Timetag::immediately().abi(), now.abi()));
    CHECK_FALSE(clockwork_timetag_before(now.abi(), Timetag::immediately().abi()));
    CHECK_FALSE(clockwork_timetag_before(Timetag().abi(), Timetag::immediately().abi()));
}

TEST_CASE("timetag: an offset is signed, and moving by one gives it back", "[timetag]") {
    const Timetag t = Timetag::fromNtpSeconds(kNtp2026);
    const int64_t ms = static_cast<int64_t>(kSecond / 1000);
    const Timetag later = t.plus(20 * ms);
    const Timetag earlier = t.plus(-20 * ms);

    CHECK(later.offsetFrom(t) == 20 * ms);
    CHECK(later - t == 20 * ms);
    CHECK(earlier - t == -20 * ms);
    CHECK(t + 20 * ms == later);
    CHECK(t - 20 * ms == earlier);
    // Subtracted as unsigned, this is a number near 2^64.
    CHECK(earlier.offsetFrom(t) == -20 * ms);
    CHECK(earlier < t);
    CHECK(t < later);
    CHECK(clockwork_timetag_offset(t.abi(), earlier.abi()) == -20 * ms);
    CHECK(clockwork_timetag_add(t.abi(), -20 * ms).ntp == earlier.ntp());
    CHECK(std::abs(later.secondsFrom(t) - 0.020) < 1e-6);
    CHECK(std::abs(earlier.secondsFrom(t) + 0.020) < 1e-6);
}

TEST_CASE("timetag: order and offset hold across the NTP rollover on 7 February 2036", "[timetag]") {
    // Half a second either side of the moment the 32-bit seconds wrap to 0.
    const Timetag before = Timetag::fromNtp(0xFFFFFFFF'80000000ull);
    const Timetag after = before.plus(static_cast<int64_t>(kSecond));
    REQUIRE(after.ntp() == 0x00000000'80000000ull);

    // Read as unsigned, `after` is the smaller number.
    REQUIRE(after.ntp() < before.ntp());
    CHECK(before < after);
    CHECK(after.offsetFrom(before) == static_cast<int64_t>(kSecond));
    CHECK(after - before == static_cast<int64_t>(kSecond));
    CHECK(before.offsetFrom(after) == -static_cast<int64_t>(kSecond));
    CHECK(after.secondsFrom(before) == 1.0);
    CHECK(clockwork_timetag_before(before.abi(), after.abi()));
    CHECK_FALSE(clockwork_timetag_before(after.abi(), before.abi()));
}

TEST_CASE("timetag: the C helpers and the C++ operators agree", "[timetag]") {
    const Timetag t = Timetag::fromNtpSeconds(kNtp2026);
    const std::vector<Timetag> all = {
        Timetag(), Timetag::immediately(), t, t.plus(1), t.plus(-1),
        t.plus(static_cast<int64_t>(kSecond) * 3600), Timetag::fromNtp(0xFFFFFFFF'FFFFFFFFull),
        Timetag::fromNtp(0x00000001'00000000ull),
    };
    for (const Timetag a : all) {
        for (const Timetag b : all) {
            INFO("a " << a.ntp() << ", b " << b.ntp());
            CHECK((clockwork_timetag_before(a.abi(), b.abi()) != 0) == (a < b));
            if (!a.isImmediate() && !b.isImmediate())
                CHECK(clockwork_timetag_offset(b.abi(), a.abi()) == a.offsetFrom(b));
        }
    }
}

TEST_CASE("timetag: NTP seconds pack as whole seconds over a 32-bit fraction", "[timetag]") {
    CHECK(Timetag::fromNtpSeconds(kNtp2026).ntp() == 3976214400ull << 32);
    CHECK(Timetag::fromNtpSeconds(kNtp2026 + 0.5).ntp() == ((3976214400ull << 32) | 0x80000000u));
    CHECK(Timetag::fromNtp((3976214400ull << 32) | 0x40000000u).ntpSeconds() == kNtp2026 + 0.25);
}
