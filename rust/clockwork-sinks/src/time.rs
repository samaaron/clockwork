// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The one clock a sink reads, and the two facts about it worth stating.
//!
//! `src/clockwork_event_sink.h` says `when` is "an OSC timetag in the same domain as
//! `dsp_process`'s block_time". That domain is wall clock: clockwork builds
//! `block_time` with `ntpToOscTimetag(wallClockNTP())` (`src/clock/clock_math.h`),
//! which is `system_clock::now()` plus the 1900→1970 offset, packed 32.32. So
//! a sink can ask the same question independently and get a comparable answer,
//! and does — a sink deciding whether a message is due must not have to be
//! handed the block clock by whoever happens to be calling it.
//!
//! # It is a `u64` for comparison and an `i64` at the ABI
//!
//! NTP seconds for any date after 1968 exceed 2^31, so the packed timetag has
//! its top bit set and is negative when read as the `int64_t` the header
//! declares. `1` means "immediately" and is a tiny positive number. Compare
//! those two signed and "immediately" sorts AFTER a real time, which would
//! deliver every scheduled message ahead of every immediate one. Unsigned, 1
//! is smaller than every real timetag, which is the meaning OSC intends.
//!
//! # It is not monotonic, and that is the domain's problem, not the sink's
//!
//! A wall clock can step. A message held for a step backwards is delivered
//! late; one held across a step forwards is delivered early. The alternative —
//! a monotonic clock — cannot compare with a timetag a client sent, which is
//! the entire content of `when`. Clockwork has one time domain and this is
//! it.

/// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
/// `src/clock/clock_math.h`'s `kNtpEpochOffset`, in the units this side needs.
const NTP_EPOCH_OFFSET_SECS: u64 = 2_208_988_800;

/// The OSC timetag value meaning "immediately". Anything at or below this is
/// due the moment it is seen and can never be late — there was no time to miss.
pub const IMMEDIATE: u64 = 1;

/// Now, as an OSC 32.32 timetag. The same value `src/clock/clock_math.h`'s
/// `ntpToOscTimetag(wallClockNTP())` would produce.
pub fn now() -> u64 {
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => {
            let secs = d.as_secs() + NTP_EPOCH_OFFSET_SECS;
            // The fraction as 32 bits: nanoseconds scaled by 2^32 / 1e9. Done
            // in u128 so the multiply cannot overflow before the divide.
            let frac = ((d.subsec_nanos() as u128) << 32) / 1_000_000_000u128;
            (secs << 32) | (frac as u64)
        }
        // Before 1970. Nothing sensible to say; "immediately" is the safest
        // answer, because it makes everything due rather than everything held.
        Err(_) => IMMEDIATE,
    }
}

/// Is a message with timetag `when` due at `now`?
#[inline]
pub fn is_due(when: u64, now: u64) -> bool {
    when <= IMMEDIATE || when <= now
}

/// Was `when` already past by `now`? Immediate messages never are: 1 does not
/// name a moment, so no moment can have been missed.
#[inline]
pub fn is_past(when: u64, now: u64) -> bool {
    when > IMMEDIATE && when < now
}

/// How many whole seconds and 32-bit fractions from `now` to `when`, as a
/// duration to wait — `None` if `when` is already due. Used by the drain to
/// decide how long to sleep rather than how often to look.
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
pub const LOOKAHEAD_UNITS: u64 = (1u64 << 32) / 100;   // 10ms in OSC 32.32 units

/// Is `when` close enough to hand to a platform scheduler? Immediate messages
/// always are — there is nothing to wait for.
pub fn is_within_lookahead(when: u64, now: u64) -> bool {
    if when <= IMMEDIATE {
        return true;
    }
    when <= now.saturating_add(LOOKAHEAD_UNITS)
}

pub fn until(when: u64, now: u64) -> Option<std::time::Duration> {
    if is_due(when, now) { return None; }
    let delta = when - now;              // checked by is_due
    let secs = delta >> 32;
    let nanos = (((delta & 0xffff_ffff) as u128) * 1_000_000_000u128) >> 32;
    Some(std::time::Duration::new(secs, nanos as u32))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn immediately_is_before_every_real_time() {
        // The whole reason the comparison is unsigned. A timetag for now is a
        // negative i64 and must still sort after 1.
        let n = now();
        assert!(n > IMMEDIATE, "a real timetag must exceed 'immediately'");
        assert!((n as i64) < 0, "a 2026 timetag is negative as an i64 — if this \
                                 ever fails the signed/unsigned trap is gone and \
                                 the comment above can go with it");
        assert!(is_due(IMMEDIATE, n));
        assert!(!is_past(IMMEDIATE, n), "'immediately' can never be late");
    }

    #[test]
    fn now_agrees_with_the_harness_packing() {
        // ntpToOscTimetag(wallClockNTP()): whole seconds in the high 32 bits,
        // counted from 1900. Checked against the Unix epoch the long way round.
        let n = now();
        let unix = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap().as_secs();
        let ntp_secs = n >> 32;
        assert!(ntp_secs.abs_diff(unix + NTP_EPOCH_OFFSET_SECS) <= 1,
                "packed seconds {ntp_secs} vs expected {}", unix + NTP_EPOCH_OFFSET_SECS);
    }

    #[test]
    fn until_converts_a_timetag_delta_to_a_wait() {
        let base = now();
        // Half a second in 32.32.
        let half = base + (1u64 << 31);
        let d = until(half, base).expect("half a second away is not due");
        assert!(d.as_millis() >= 499 && d.as_millis() <= 501, "{d:?}");
        assert!(until(base, base).is_none(), "now is due now");
        assert!(until(base - 1, base).is_none(), "the past is due now");
    }
}
