// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `src/clockwork_timetag.h`: an OSC timetag, NTP 32.32, as the C struct the
//! boundary passes. For ordering and offsets, `clockwork_osc::Timetag` holds
//! the same 64 bits with the header's rules.

/// `ClockworkTimetag`. A C struct, not a bare `u64`: on 32-bit x86 Linux C
/// returns the struct through memory.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClockworkTimetag {
    pub ntp: u64,
}

impl ClockworkTimetag {
    /// OSC's "immediately".
    pub const IMMEDIATELY: ClockworkTimetag = ClockworkTimetag { ntp: 1 };
}
