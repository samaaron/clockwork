// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! An OSC timetag, as a type of its own: `ClockworkTimetag` in
//! `src/clockwork_timetag.h`, with the same rules.
//!
//! The value is NTP time in 32.32 fixed point (RFC 5905 sec. 6), the 64 bits
//! OSC carries. Immediately (OSC's 1, and 0, which Clockwork reads the same
//! way) comes before every real time. Real times order by serial-number
//! arithmetic (RFC 1982): `a` comes before `b` when `a - b`, wrapped to 64 bits
//! and read as signed, is negative, which holds across the NTP era rollover in
//! 2036. It converts to and from an integer only by name, so it is never
//! compared or subtracted as one.

use core::cmp::Ordering;

/// One second, in timetag units.
pub const SECOND: u64 = 1 << 32;

#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Timetag(u64);

impl Timetag {
    /// OSC's "immediately".
    pub const IMMEDIATELY: Timetag = Timetag(1);

    pub const fn from_ntp(ntp: u64) -> Timetag {
        Timetag(ntp)
    }

    pub const fn ntp(self) -> u64 {
        self.0
    }

    pub const fn is_immediate(self) -> bool {
        self.0 <= 1
    }

    /// How far this is from `from`, in 32.32 seconds: positive when this is
    /// later. Both are real times, not immediately.
    pub const fn offset_from(self, from: Timetag) -> i64 {
        self.0.wrapping_sub(from.0) as i64
    }

    pub fn seconds_from(self, from: Timetag) -> f64 {
        self.offset_from(from) as f64 / SECOND as f64
    }

    /// This moved by `offset` 32.32 seconds.
    pub const fn plus(self, offset: i64) -> Timetag {
        Timetag(self.0.wrapping_add(offset as u64))
    }
}

impl PartialEq for Timetag {
    fn eq(&self, other: &Timetag) -> bool {
        self.cmp(other) == Ordering::Equal
    }
}

impl Eq for Timetag {}

impl PartialOrd for Timetag {
    fn partial_cmp(&self, other: &Timetag) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for Timetag {
    fn cmp(&self, other: &Timetag) -> Ordering {
        match (self.is_immediate(), other.is_immediate()) {
            (true, true) => Ordering::Equal,
            (true, false) => Ordering::Less,
            (false, true) => Ordering::Greater,
            (false, false) => self.offset_from(*other).cmp(&0),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // 1 January 2026, 00:00 UTC.
    const T2026: Timetag = Timetag::from_ntp(3_976_214_400 << 32);

    #[test]
    fn immediately_comes_before_every_real_time_and_0_and_1_are_both_immediately() {
        // Read as i64, every real time is negative and 1 sorts after it.
        assert!((T2026.ntp() as i64) < 1);
        assert!(Timetag::IMMEDIATELY < T2026);
        assert!(Timetag::default() < T2026);
        assert_eq!(Timetag::default(), Timetag::IMMEDIATELY);
        assert!(!Timetag::from_ntp(2).is_immediate());
    }

    #[test]
    fn an_offset_is_signed_and_moving_by_one_gives_it_back() {
        let ms = (SECOND / 1000) as i64;
        let earlier = T2026.plus(-20 * ms);
        assert_eq!(earlier.offset_from(T2026), -20 * ms);
        assert_eq!(T2026.plus(20 * ms).offset_from(T2026), 20 * ms);
        assert!(earlier < T2026);
        assert!((earlier.seconds_from(T2026) + 0.020).abs() < 1e-6);
    }

    #[test]
    fn order_and_offset_hold_across_the_ntp_rollover_in_2036() {
        let before = Timetag::from_ntp(0xFFFF_FFFF_8000_0000);
        let after = before.plus(SECOND as i64);
        assert_eq!(after.ntp(), 0x0000_0000_8000_0000);
        // Read as unsigned, `after` is the smaller number.
        assert!(after.ntp() < before.ntp());
        assert!(before < after);
        assert_eq!(after.offset_from(before), SECOND as i64);
        assert_eq!(after.seconds_from(before), 1.0);
    }

    // The same vectors as test/test_timetag.cpp's agreement case, so the two
    // languages order alike.
    #[test]
    fn it_orders_as_the_c_helpers_do() {
        let all = [
            Timetag::default(), Timetag::IMMEDIATELY, T2026, T2026.plus(1), T2026.plus(-1),
            T2026.plus(SECOND as i64 * 3600), Timetag::from_ntp(u64::MAX),
            Timetag::from_ntp(0x0000_0001_0000_0000),
        ];
        // clockwork_timetag_before, spelled out.
        let before = |a: Timetag, b: Timetag| {
            if a.is_immediate() {
                return !b.is_immediate();
            }
            if b.is_immediate() {
                return false;
            }
            (a.ntp().wrapping_sub(b.ntp()) as i64) < 0
        };
        for a in all {
            for b in all {
                assert_eq!(before(a, b), a < b, "a {:x}, b {:x}", a.ntp(), b.ntp());
            }
        }
    }
}
