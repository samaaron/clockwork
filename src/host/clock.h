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

#include <chrono>
#include <cstdint>

namespace clockwork_host {

inline int64_t ntp_to_osc_timetag(double ntp_seconds) {
    return clockwork::ntpToOscTimetag(ntp_seconds);
}

// Current time as an OSC timetag from the system clock.
inline int64_t osc_now() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    int64_t us = duration_cast<microseconds>(now).count();
    double unix_seconds = static_cast<double>(us) / 1'000'000.0;
    return ntp_to_osc_timetag(unix_seconds + clockwork::kNtpEpochOffset);
}

}  // namespace clockwork_host
