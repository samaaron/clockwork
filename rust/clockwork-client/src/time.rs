// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! NTP time, which is the engine's.
//!
//! The clock snapshot ([`crate::Clock`]) is anchored in NTP seconds, and an
//! OSC bundle's timetag is NTP in 32.32 fixed point. A host that schedules
//! against the engine needs "now" in both forms and the arithmetic between
//! them, and every host that has written it has written it slightly
//! differently. This is it, once, with the epoch offset spelled out.

use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// Seconds from the NTP epoch (1900) to the Unix one (1970).
pub const NTP_UNIX_OFFSET_SECONDS: u64 = 2_208_988_800;

/// One second, in 32.32 fixed point.
pub const TIMETAG_SECOND: u64 = 1 << 32;

fn since_unix() -> Duration {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default()
}

/// Now, as NTP seconds — the domain of [`crate::Clock::beat_at`].
pub fn ntp_now() -> f64 {
    since_unix().as_secs_f64() + NTP_UNIX_OFFSET_SECONDS as f64
}

/// Now, as an OSC timetag (NTP 32.32). Exact integer arithmetic, so two
/// calls a nanosecond apart differ by at most one tick.
pub fn timetag_now() -> u64 {
    let d = since_unix();
    let secs = d.as_secs() + NTP_UNIX_OFFSET_SECONDS;
    let frac = (u64::from(d.subsec_nanos()) << 32) / 1_000_000_000;
    (secs << 32) | frac
}

/// NTP seconds to a timetag. The fraction is truncated to the tick.
pub fn timetag_from_ntp(seconds: f64) -> u64 {
    if seconds <= 0.0 {
        return 0;
    }
    let whole = seconds.floor();
    let frac = ((seconds - whole) * TIMETAG_SECOND as f64) as u64;
    ((whole as u64) << 32) | frac.min(u64::from(u32::MAX))
}

/// A timetag to NTP seconds.
pub fn timetag_from_ntp_inverse(timetag: u64) -> f64 {
    ntp_from_timetag(timetag)
}

/// A timetag to NTP seconds.
pub fn ntp_from_timetag(timetag: u64) -> f64 {
    (timetag >> 32) as f64 + (timetag & 0xffff_ffff) as f64 / TIMETAG_SECOND as f64
}

/// A timetag moved by `seconds`, either way. What a host does to anchor a
/// piece a little into the future: `timetag_offset(timetag_now(), 0.5)`.
pub fn timetag_offset(timetag: u64, seconds: f64) -> u64 {
    timetag.wrapping_add_signed((seconds * TIMETAG_SECOND as f64) as i64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn now_is_after_2026_in_both_forms_and_they_agree() {
        let ntp_2026 = (56 * 365 + 14) as f64 * 86_400.0 + NTP_UNIX_OFFSET_SECONDS as f64;
        let a = ntp_now();
        let t = timetag_now();
        let b = ntp_now();
        assert!(a > ntp_2026, "{a}");
        let tt = ntp_from_timetag(t);
        assert!(tt >= a - 1e-6 && tt <= b + 1e-6, "{a} <= {tt} <= {b}");
    }

    #[test]
    fn timetags_round_trip_to_the_tick() {
        for s in [0.0, 1.0, 1.5, 3_950_000_000.123_456, 4_000_000_000.999_999] {
            let t = timetag_from_ntp(s);
            let back = ntp_from_timetag(t);
            assert!((back - s).abs() < 1.0 / TIMETAG_SECOND as f64 * 2.0 + 1e-6, "{s} -> {t} -> {back}");
        }
        assert_eq!(timetag_from_ntp(-5.0), 0);
        assert_eq!(timetag_from_ntp(1.0), TIMETAG_SECOND);
        assert_eq!(timetag_from_ntp_inverse(TIMETAG_SECOND * 3), 3.0);
    }

    #[test]
    fn an_offset_moves_by_whole_seconds_exactly() {
        let t = timetag_from_ntp(100.25);
        assert_eq!(timetag_offset(t, 0.5), timetag_from_ntp(100.75));
        assert_eq!(timetag_offset(t, -0.25), timetag_from_ntp(100.0));
        assert_eq!(timetag_offset(t, 2.0) - t, 2 * TIMETAG_SECOND);
    }
}
