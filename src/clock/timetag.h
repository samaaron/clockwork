// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * timetag.h — clockwork::Timetag, the engine's OSC timetag.
 *
 * ClockworkTimetag (src/clockwork_timetag.h) with C++ operators and the same
 * rules: immediately comes first, and real times order by serial-number
 * arithmetic (RFC 1982), so the order holds across the NTP era rollover in
 * 2036. It converts to and from an integer only by name (fromNtp, ntp), so
 * `when <= 1` and `when - blockTime` do not compile.
 */
#pragma once

#include "clockwork_timetag.h"

#include <compare>
#include <cstdint>

namespace clockwork {

class Timetag {
public:
    // Immediately (0).
    constexpr Timetag() = default;

    static constexpr Timetag fromNtp(uint64_t ntp) {
        Timetag t;
        t.mNtp = ntp;
        return t;
    }
    static constexpr Timetag fromAbi(ClockworkTimetag t) { return fromNtp(t.ntp); }
    static constexpr Timetag immediately() { return fromNtp(1); }

    // NTP seconds since 1900. Not a round trip to the bit: the fraction is 32
    // bits and a double's mantissa 53.
    static constexpr Timetag fromNtpSeconds(double ntpSeconds) {
        const auto s = static_cast<uint32_t>(ntpSeconds);
        const auto f = static_cast<uint32_t>((ntpSeconds - s) * kUnitsPerSecond);
        return fromNtp((static_cast<uint64_t>(s) << 32u) | f);
    }
    constexpr double ntpSeconds() const {
        return static_cast<double>(mNtp >> 32u) + static_cast<double>(mNtp & 0xFFFFFFFFu) / kUnitsPerSecond;
    }

    constexpr uint64_t ntp() const { return mNtp; }
    constexpr ClockworkTimetag abi() const { return ClockworkTimetag{mNtp}; }
    constexpr bool isImmediate() const { return mNtp <= 1; }

    // How far this is from `from`, in 32.32 seconds: positive when this is
    // later. Both are real times, not immediately.
    constexpr int64_t offsetFrom(Timetag from) const { return static_cast<int64_t>(mNtp - from.mNtp); }
    constexpr double secondsFrom(Timetag from) const {
        return static_cast<double>(offsetFrom(from)) / kUnitsPerSecond;
    }
    // This moved by `offset` 32.32 seconds.
    constexpr Timetag plus(int64_t offset) const { return fromNtp(mNtp + static_cast<uint64_t>(offset)); }

    // Weak, not strong: 0 and 1 are both immediately, and equal.
    friend constexpr std::weak_ordering operator<=>(Timetag a, Timetag b) {
        if (a.isImmediate() || b.isImmediate()) return b.isImmediate() <=> a.isImmediate();
        return a.offsetFrom(b) <=> 0;
    }
    friend constexpr bool operator==(Timetag a, Timetag b) { return (a <=> b) == 0; }

    static constexpr double kUnitsPerSecond = 4294967296.0;   // 2^32

private:
    uint64_t mNtp = 0;
};

}  // namespace clockwork
