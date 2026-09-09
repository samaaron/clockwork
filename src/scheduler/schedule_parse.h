// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork
 * Copyright (c) 2025 Sam Aaron
 *
 *
 * schedule_parse.h — the two ways to schedule, parsed in one place so the engine
 * and the standalone host agree on the wire form:
 *   - a timestamped OSC bundle ("#bundle" + an 8-byte timetag), and
 *   - "/clockwork/schedule <timetag> <blob>", the flat twin (a single addressed inner
 *     message the scheduler re-ingests on time).
 * Pure byte work — no synth, no engine globals.
 *
 * The parsing itself is Rust (rust/clockwork-schedule/src/parse.rs), reached
 * through clockwork_schedule.h; what remains here is the C++ spelling of it. The
 * property that matters survived the move: every byte offset is derived from
 * the reserved prefix, on that side from `clockwork_sys!("schedule")` as it was here
 * from CLOCKWORK_SYS(...), so the parser cannot fall out of step with the prefix. It
 * once could, and did.
 */
#pragma once

#include <cstdint>

#include "clockwork_schedule.h"

// Kept although nothing below spells either any more: this header's includers
// have always reached the reserved prefix and the shared clock arithmetic
// through it (test_schedule_parse.cpp asserts clockwork_ntp_to_timetag against
// clockwork::ntpToOscTimetag), and quietly dropping them from a header is a
// change to its contract rather than a tidy-up.
#include "clockwork_prefix.h"
#include "../clock/clock_math.h"

// NTP-seconds (double) → OSC int64 timetag (seconds<<32 | fraction).
// The same formula as clockwork::ntpToOscTimetag in clock_math.h, which
// test_schedule_parse.cpp checks by comparing the two results directly — so
// that assertion now also compares this side of the ABI with the other.
inline int64_t clockwork_ntp_to_timetag(double ntp) {
    return clockwork_sched_ntp_to_timetag(ntp);
}

// A timestamped OSC bundle: "#bundle" + an 8-byte timetag.
inline bool clockwork_is_bundle(const uint8_t* data, uint32_t size) {
    return clockwork_sched_is_bundle(data, size) != 0;
}

// The 8-byte NTP timetag at offset 8 of a bundle.
inline uint64_t clockwork_bundle_timetag(const uint8_t* bundle) {
    return clockwork_sched_bundle_timetag(bundle);
}

struct SchedulePacket {
    bool           ok      = false;
    int64_t        when    = 0;        // OSC timetag
    const uint8_t* blob    = nullptr;  // inner OSC to re-dispatch on time
    uint32_t       blobLen = 0;
};

// Parse "/clockwork/schedule <timetag> <blob>". `timetag` is the OSC int64 'h' (full
// sub-sample resolution) or, as a convenience, a 'd'/'f' NTP-seconds value.
inline SchedulePacket clockwork_parse_schedule(const uint8_t* msg, uint32_t size) {
    const ClockworkSchedulePacket p = clockwork_sched_parse(msg, size);
    SchedulePacket r;
    if (!p.ok) return r;   // a failed parse is wholly inert, not partly filled
    r.ok      = true;
    r.when    = p.when;
    r.blob    = p.blob;
    r.blobLen = p.blob_len;
    return r;
}
