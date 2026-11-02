// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The one clock a sink reads, and the two facts about it worth stating.
//!
//! `src/clockwork_event_sink.h` says `when` is "an OSC timetag in the same domain as
//! `dsp_process`'s block_time". That domain is wall clock: clockwork builds
//! `block_time` from `system_clock::now()` plus the 1900→1970 offset, packed
//! 32.32. So a sink can ask the same question independently and get a
//! comparable answer, and does — a sink deciding whether a message is due
//! must not have to be handed the block clock by whoever happens to be
//! calling it.
//!
//! # It is a `Timetag`
//!
//! `clockwork_osc::Timetag`: immediately comes before every real time, and
//! real times order by serial-number arithmetic, across the NTP rollover in
//! 2036. Read as an `i64` every real time is negative and "immediately" would
//! sort after it; read as a `u64` an earlier time minus a later one is near
//! 2^64. The type does neither.
//!
//! # It is not monotonic, and that is the domain's problem, not the sink's
//!
//! A wall clock can step. A message held for a step backwards is delivered
//! late; one held across a step forwards is delivered early. The alternative —
//! a monotonic clock — cannot compare with a timetag a client sent, which is
//! the entire content of `when`. Clockwork has one time domain and this is
//! it.

use clockwork_osc::Timetag;

/// Now, as an OSC timetag: `clockwork_osc::osc::now_timetag`.
pub fn now() -> Timetag {
    Timetag::from_ntp(clockwork_osc::osc::now_timetag())
}

/// Is a message with timetag `when` due at `now`?
#[inline]
pub fn is_due(when: Timetag, now: Timetag) -> bool {
    when.is_immediate() || when <= now
}

/// Was `when` already past by `now`? Immediate messages never are: immediately
/// does not name a moment, so no moment can have been missed.
#[inline]
pub fn is_past(when: Timetag, now: Timetag) -> bool {
    !when.is_immediate() && when < now
}

/// The look-ahead: how far before its time a message may be handed to a
/// platform that schedules for itself.
///
/// The trade this number IS: once a message is with the kernel or the MIDI
/// server, we cannot take it back, so everything handed over early is
/// uncancellable early. Hold it instead and cancellation stays possible but the
/// timing becomes our thread's wakeup rather than the platform's timer.
///
/// 10ms, the same window MidiClockOut uses for the same reason. Long enough
/// that the platform does the fine timing; short enough that a flush cancels
/// all but the last instant.
pub const LOOKAHEAD_UNITS: i64 = (1i64 << 32) / 100;   // 10ms in OSC 32.32 units

/// Is `when` close enough to hand to a platform scheduler? Immediate messages
/// always are — there is nothing to wait for.
pub fn is_within_lookahead(when: Timetag, now: Timetag) -> bool {
    when.is_immediate() || when <= now.plus(LOOKAHEAD_UNITS)
}

/// How long from `now` to `when`, as a duration to wait — `None` if `when` is
/// already due. Used by the drain to decide how long to sleep rather than how
/// often to look.
pub fn until(when: Timetag, now: Timetag) -> Option<std::time::Duration> {
    if is_due(when, now) { return None; }
    let delta = when.offset_from(now) as u64;   // positive: checked by is_due
    let secs = delta >> 32;
    let nanos = (((delta & 0xffff_ffff) as u128) * 1_000_000_000u128) >> 32;
    Some(std::time::Duration::new(secs, nanos as u32))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn immediately_is_due_and_never_late() {
        let n = now();
        assert!(Timetag::IMMEDIATELY < n);
        assert!(is_due(Timetag::IMMEDIATELY, n));
        assert!(!is_past(Timetag::IMMEDIATELY, n), "'immediately' can never be late");
        assert!(is_within_lookahead(Timetag::IMMEDIATELY, n));
    }

    #[test]
    fn until_converts_a_timetag_delta_to_a_wait() {
        let base = now();
        // Half a second in 32.32.
        let half = base.plus(1 << 31);
        let d = until(half, base).expect("half a second away is not due");
        assert!(d.as_millis() >= 499 && d.as_millis() <= 501, "{d:?}");
        assert!(until(base, base).is_none(), "now is due now");
        assert!(until(base.plus(-1), base).is_none(), "the past is due now");
    }

    #[test]
    fn a_wait_across_the_2036_rollover_is_the_time_between_not_the_numbers() {
        let before = Timetag::from_ntp(0xFFFF_FFFF_8000_0000);
        let after = before.plus(1 << 32);
        assert!(after.ntp() < before.ntp());
        assert_eq!(until(after, before), Some(std::time::Duration::from_secs(1)));
        assert!(is_due(before, after));
        assert!(is_past(before, after));
    }
}
