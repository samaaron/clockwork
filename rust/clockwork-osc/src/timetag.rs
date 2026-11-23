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
//! compared with one. As with `std::time::Instant`, a time plus an offset is
//! a time, and a time minus a time is a signed offset, in 32.32 seconds.

use core::cmp::Ordering;
use core::ops::{Add, Sub};

/// One second, in timetag units.
pub const SECOND: u64 = 1 << 32;

/// `#[repr(C)]`, so it is `ClockworkTimetag` at the C boundary, passed and
/// returned as C passes and returns that struct.
#[repr(C)]
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

    /// NTP seconds since 1900, packed as whole seconds over a 32-bit
    /// fraction. Rust's float-to-integer casts saturate, so a value outside
    /// the 32-bit range clamps; for every value in range this is
    /// bit-identical to `clockwork::Timetag::fromNtpSeconds`.
    pub fn from_ntp_seconds(ntp: f64) -> Timetag {
        let s = ntp as u32;
        let f = ((ntp - s as f64) * SECOND as f64) as u32;
        Timetag(((s as u64) << 32) | f as u64)
    }

    /// NTP seconds since 1900. Not a round trip to the bit: the fraction is
    /// 32 bits and an f64's mantissa 53.
    pub fn ntp_seconds(self) -> f64 {
        (self.0 >> 32) as f64 + (self.0 & 0xFFFF_FFFF) as f64 / SECOND as f64
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

impl Add<i64> for Timetag {
    type Output = Timetag;
    fn add(self, offset: i64) -> Timetag {
        self.plus(offset)
    }
}

impl Sub<i64> for Timetag {
    type Output = Timetag;
    fn sub(self, offset: i64) -> Timetag {
        self.plus(offset.wrapping_neg())
    }
}

/// The signed offset from `from` to `self`: [`Timetag::offset_from`].
impl Sub for Timetag {
    type Output = i64;
    fn sub(self, from: Timetag) -> i64 {
        self.offset_from(from)
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
    fn ntp_seconds_pack_as_whole_seconds_over_a_32_bit_fraction() {
        assert_eq!(Timetag::from_ntp_seconds(0.0).ntp(), 0);
        assert_eq!(Timetag::from_ntp_seconds(1.0).ntp(), 1 << 32);
        assert_eq!(Timetag::from_ntp_seconds(1.5).ntp(), (1 << 32) | 0x8000_0000);
        assert_eq!(Timetag::from_ntp_seconds(2.25).ntp(), (2 << 32) | 0x4000_0000);
        assert_eq!(Timetag::from_ntp_seconds(3_976_214_400.0), T2026);
        assert_eq!(Timetag::from_ntp((2 << 32) | 0x4000_0000).ntp_seconds(), 2.25);
        // Out of range clamps rather than wrapping.
        assert_eq!(Timetag::from_ntp_seconds(-1.0).ntp(), 0);
    }

    #[test]
    fn an_offset_is_signed_and_moving_by_one_gives_it_back() {
        let ms = (SECOND / 1000) as i64;
        let earlier = T2026.plus(-20 * ms);
        assert_eq!(earlier.offset_from(T2026), -20 * ms);
        assert_eq!(earlier - T2026, -20 * ms);
        assert_eq!(T2026 - 20 * ms, earlier);
        assert_eq!(T2026 + 20 * ms - T2026, 20 * ms);
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

/// Now, as an OSC 32.32 fixed-point timetag.
///
/// Here because this crate owns the timetag type and is the one thing both the
/// MIDI and the sink crates already depend on — putting it in either would
/// make the other depend on it, and clockwork-sinks already depends on clockwork-midi for
/// its endpoint, so that edge cannot run the other way.
///
/// The SAME formula as `clockwork::Timetag::fromNtpSeconds(wallClockNTP())`
/// in C++; the sinks read the clock through this one. The tests assert the
/// two languages agree.
#[cfg(feature = "std")]
pub fn now_timetag() -> u64 {
    timetag_of(std::time::SystemTime::now())
}

/// A moment of the system clock, as an OSC 32.32 timetag: what an OS's own
/// timestamp on an event (gilrs's, a SystemTime) becomes on the wire.
#[cfg(feature = "std")]
pub fn timetag_of(t: std::time::SystemTime) -> u64 {
    // Seconds between the NTP epoch (1900) and the Unix epoch (1970).
    const NTP_EPOCH_OFFSET: u64 = 2_208_988_800;
    let d = t.duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
    let secs = d.as_secs() + NTP_EPOCH_OFFSET;
    // 32 bits of seconds over 32 bits of fraction (RFC 5905 sec. 6).
    let frac = ((d.subsec_nanos() as u64) << 32) / 1_000_000_000;
    (secs << 32) | frac
}

/// How long from now until `when`, or `None` if it is already due or is
/// immediately.
#[cfg(feature = "std")]
pub fn delay_until(when: Timetag) -> Option<std::time::Duration> {
    let now = Timetag::from_ntp(now_timetag());
    if when.is_immediate() || when <= now {
        return None;
    }
    let diff = (when - now) as u64;   // positive: checked above
    let secs = diff >> 32;
    let nanos = (((diff & 0xFFFF_FFFF) * 1_000_000_000) >> 32) as u32;
    Some(std::time::Duration::new(secs, nanos))
}

#[cfg(all(test, feature = "std"))]
mod clock_tests {
    use super::*;

    #[test]
    fn a_system_time_is_its_ntp_timetag() {
        // A second and a half past the Unix epoch: the NTP epoch's offset in
        // seconds, then half a second as the fraction.
        let t = std::time::UNIX_EPOCH + std::time::Duration::from_millis(1500);
        assert_eq!(timetag_of(t), ((2_208_988_800u64 + 1) << 32) | 0x8000_0000);
    }

    #[test]
    fn an_immediate_sentinel_is_never_a_delay() {
        assert!(delay_until(Timetag::from_ntp(0)).is_none());
        assert!(delay_until(Timetag::IMMEDIATELY).is_none());
    }

    #[test]
    fn a_past_timetag_is_due_not_negative() {
        let now = Timetag::from_ntp(now_timetag());
        assert!(delay_until(now - (1i64 << 32)).is_none(), "a second ago is due");
    }

    #[test]
    fn a_future_timetag_converts_to_about_that_long() {
        // delay_until reads the clock itself, somewhere between the two reads
        // here: the delay is a second less however long that took — measured,
        // so a machine that pauses moves the allowance with it.
        let before = std::time::Instant::now();
        let now = Timetag::from_ntp(now_timetag());
        let d = delay_until(now + (1i64 << 32)).expect("a second out is a delay");
        let took = before.elapsed();
        let ms = d.as_millis();
        let slack = 2 + took.as_millis();   // + the timetag's own rounding
        assert!(ms <= 1000 + 2 && ms + slack >= 1000,
                "one second less {took:?}, got {ms}ms");
    }
}
