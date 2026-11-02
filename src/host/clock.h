// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron


    Wall-clock OSC time for the standalone host. Timetags are NTP (seconds since
    1900) packed as a 64-bit fixed-point value, matching the domain a client uses
    for its scheduled /clockwork/schedule messages.
*/

#pragma once

#include "clock/clock_math.h"
#include "clock/timetag.h"

#include <chrono>
#include <cstdint>

namespace clockwork_host {

// Current time as an OSC timetag from the system clock.
inline clockwork::Timetag osc_now() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    int64_t us = duration_cast<microseconds>(now).count();
    double unix_seconds = static_cast<double>(us) / 1'000'000.0;
    return clockwork::Timetag::fromNtpSeconds(unix_seconds + clockwork::kNtpEpochOffset);
}

// Converts the difference between two timetags (`ticks`, a time minus a
// time, in units of
// 1/2^32 s) to microseconds, rounded up so a wait for it ends at or after the
// later timetag. Returns 0 for a difference that is not positive.
inline std::chrono::microseconds osc_interval(int64_t ticks) {
    if (ticks <= 0) return std::chrono::microseconds(0);
    const uint64_t t = static_cast<uint64_t>(ticks);
    const uint64_t whole = t >> 32;
    const uint64_t frac = ((t & 0xffffffffu) * 1'000'000 + 0xffffffffu) >> 32;
    return std::chrono::microseconds(static_cast<int64_t>(whole * 1'000'000 + frac));
}

}  // namespace clockwork_host
