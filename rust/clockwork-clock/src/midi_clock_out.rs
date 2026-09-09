// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! MIDI clock out: which pulses a follower of a timeline owes.
//!
//! A follower emits one 0xF8 per 1/24 beat of a [`Timeline`], on its grid:
//! pulse `k` is due at `time_at_beat(k / 24)`. All the follower remembers is
//! the index of the next pulse it has not yet handed over; each periodic call
//! to [`due`] answers "which indices from there fall inside `now + horizon`",
//! against a fresh snapshot of the timeline. A tempo change therefore re-pins
//! every pulse not yet handed over onto the new grid with nothing to update,
//! and an index is never answered twice.
//!
//! THE BEAT IS THE PULSE COUNT — the same rule the inbound estimator in
//! [`crate::midi`] follows — so nothing is skipped or repeated to chase
//! time: a pulse whose time has passed is still owed (it goes out late), up
//! to the point where catching up would be a burst no receiver could follow.
//! Past that the follower re-syncs to the first pulse after `now`, and the
//! receiver sees one step instead of a hundred pulses.
//!
//! Transport is not decided here. Pulses run whether or not the timeline is
//! playing — a MIDI clock always runs; Start and Stop say what to do with
//! it — and the caller compares the snapshot's `playing` flag itself.
//!
//! Every `now` comes from the caller. Nothing here reads a clock.

use crate::timeline::Timeline;

/// Standard MIDI clock: 24 pulses per quarter note.
pub const PPQN: i64 = 24;

/// A follower that has handed over nothing yet: its first pulse is the first
/// one after `now` (or after `floor`, see [`due`]).
pub const UNSTARTED: i64 = i64::MIN;

/// How far the grid may move away from the follower's next pulse, in beats,
/// before the follower re-syncs instead of catching up. A beat behind is 24
/// late pulses in a burst, which a receiver rides out; more than that is a
/// jump (a Song Position, a re-anchored session) and is answered as one.
pub const RESYNC_BEATS: f64 = 1.0;

/// The answer to [`due`]: pulses `first .. first + count`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Due {
    pub first: i64,
    pub count: u32,
}

impl Due {
    /// The index to ask from next time.
    pub fn next(self) -> i64 {
        self.first + self.count as i64
    }
}

/// The NTP time pulse `index` is due: the grid at beat `index / 24`.
pub fn pulse_ntp(t: &Timeline, index: i64) -> f64 {
    t.time_at_beat(index as f64 / PPQN as f64)
}

/// The first pulse due at or after `ntp`.
pub fn first_pulse_from(t: &Timeline, ntp: f64) -> i64 {
    (t.beat_at(ntp) * PPQN as f64).ceil() as i64
}

/// The pulses a follower owes, from `next` (or [`UNSTARTED`]) up to
/// `now + horizon`, at most `max` of them; the rest are owed next call.
///
/// `floor` is the latest time the caller has already committed to the wire
/// on any follower: a follower that starts (or re-syncs) begins after it, so
/// its pulses never have to be slotted in ahead of ones already handed over.
/// A follower that is simply continuing ignores it — its pulses are later
/// than anything committed by construction, because they were not inside
/// the previous horizon.
///
/// Re-sync: if the grid has moved more than [`RESYNC_BEATS`] away from
/// `next` in either direction, `next` is abandoned for the first pulse after
/// `now` (or `floor`). Within that, a pulse whose time has passed is still
/// owed and is answered late.
pub fn due(t: &Timeline, next: i64, floor: f64, now: f64, horizon: f64, max: u32) -> Due {
    let start = if floor > now { floor } else { now };
    let span = RESYNC_BEATS * 60.0 / t.bpm;
    let first = if next == UNSTARTED {
        first_pulse_from(t, start)
    } else {
        let at = pulse_ntp(t, next);
        if at < now - span || at > now + horizon + span {
            first_pulse_from(t, start)
        } else {
            next
        }
    };
    // Pulse k is due when time_at_beat(k / 24) <= now + horizon, i.e. when
    // k <= floor(beat_at(now + horizon) * 24).
    let last = (t.beat_at(now + horizon) * PPQN as f64).floor() as i64;
    let owed = (last - first + 1).max(0);
    let count = owed.min(max as i64) as u32;
    Due { first, count }
}

#[cfg(test)]
mod tests {
    use super::*;

    const T0: f64 = 3_900_000_000.0; // some NTP instant
    const HORIZON: f64 = 0.1;
    const NO_FLOOR: f64 = 0.0;

    /// A grid with beat `anchor_beat` at `anchor_ntp`.
    fn grid(bpm: f64, anchor_beat: f64, anchor_ntp: f64) -> Timeline {
        Timeline {
            bpm,
            anchor_beat,
            anchor_ntp,
            playing: 1,
            ..Timeline::placeholder(0)
        }
    }

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-6
    }

    #[test]
    fn pulses_land_on_the_grid() {
        // 120 BPM: a pulse every 60 / (120 * 24) = 20.8333 ms.
        let t = grid(120.0, 0.0, T0);
        let d = due(&t, UNSTARTED, NO_FLOOR, T0, HORIZON, 64);
        // T0 + 0, 20.8, 41.7, 62.5, 83.3 ms are inside 100 ms; 104.2 is not.
        assert_eq!(d, Due { first: 0, count: 5 });
        assert_eq!(d.next(), 5);
        for k in 0..5 {
            assert!(close(pulse_ntp(&t, k), T0 + k as f64 / 48.0), "pulse {k}");
        }
        assert!(close(pulse_ntp(&t, 1) - pulse_ntp(&t, 0), 0.020833333));
    }

    #[test]
    fn nothing_is_answered_twice_however_the_calls_fall() {
        // Call at a cadence finer than a pulse, then coarser, then a gap:
        // every index comes out exactly once, in order, with no hole.
        let t = grid(120.0, 0.0, T0);
        let mut next = UNSTARTED;
        let mut seen = Vec::new();
        let mut now = T0;
        for step in [0.007, 0.007, 0.007, 0.05, 0.05, 0.003, 0.09, 0.1, 0.1] {
            let d = due(&t, next, NO_FLOOR, now, HORIZON, 64);
            seen.extend(d.first..d.next());
            next = d.next();
            now += step;
        }
        let last_call = now - 0.1;
        let expected: Vec<i64> =
            (0..=((t.beat_at(last_call + HORIZON) * 24.0).floor() as i64)).collect();
        assert_eq!(seen, expected);
    }

    #[test]
    fn a_tempo_change_re_pins_the_pulses_not_yet_handed_over() {
        // 120 BPM until T0 + 0.05, then 60 BPM with the beat continuous at
        // the change (what a tempo set does to a timeline).
        let fast = grid(120.0, 0.0, T0);
        let d = due(&fast, UNSTARTED, NO_FLOOR, T0, HORIZON, 64);
        assert_eq!(d.next(), 5);

        let tc = T0 + 0.05;
        let slow = grid(60.0, fast.beat_at(tc), tc);
        // Pulse 5 is beat 5/24; on the new grid that is later than it was.
        let was = pulse_ntp(&fast, 5);
        let now_is = pulse_ntp(&slow, 5);
        assert!(now_is > was);
        assert!(close(now_is, tc + (5.0 / 24.0 - slow.anchor_beat) * 1.0));

        // The follower asks from 5 as before and gets the new times: within
        // 100 ms of tc + 0.05 at 60 BPM (41.7 ms a pulse) that is 5 and 6.
        let d = due(&slow, 5, NO_FLOOR, tc + 0.05, HORIZON, 64);
        assert_eq!(d.first, 5);
        assert!(pulse_ntp(&slow, d.first) >= tc);
        assert!(pulse_ntp(&slow, d.next() - 1) <= tc + 0.05 + HORIZON);
        assert!(pulse_ntp(&slow, d.next()) > tc + 0.05 + HORIZON);
    }

    #[test]
    fn a_stopped_transport_still_pulses() {
        let mut t = grid(120.0, 0.0, T0);
        t.playing = 0;
        let d = due(&t, UNSTARTED, NO_FLOOR, T0, HORIZON, 64);
        assert_eq!(d, Due { first: 0, count: 5 });
    }

    #[test]
    fn a_pulse_whose_time_has_passed_is_still_owed() {
        // The caller was late by 300 ms (0.6 beat at 120): the pulses in that
        // gap go out late rather than being dropped, so the count holds.
        let t = grid(120.0, 0.0, T0);
        let now = T0 + 0.3;
        let d = due(&t, 0, NO_FLOOR, now, HORIZON, 64);
        assert_eq!(d.first, 0);
        assert_eq!(
            d.next(),
            (t.beat_at(now + HORIZON) * 24.0).floor() as i64 + 1
        );
    }

    #[test]
    fn a_grid_that_jumped_ahead_is_re_synced_not_chased() {
        // Ten beats passed with no call (or the session re-anchored ten
        // beats ahead): 240 pulses in a burst would be useless, so the
        // follower steps to the first pulse after now.
        let t = grid(120.0, 0.0, T0);
        let now = T0 + 5.0;
        let d = due(&t, 0, NO_FLOOR, now, HORIZON, 64);
        assert_eq!(d.first, first_pulse_from(&t, now));
        assert_eq!(d.first, 240);
        assert_eq!(d.count, 5);
    }

    #[test]
    fn a_grid_that_jumped_back_is_re_synced_not_silent() {
        // The session re-anchored ten beats behind the follower's next pulse:
        // waiting five seconds for the grid to reach it would be silence.
        let t = grid(120.0, 0.0, T0);
        let d = due(&t, 240, NO_FLOOR, T0, HORIZON, 64);
        assert_eq!(d, Due { first: 0, count: 5 });
    }

    #[test]
    fn within_a_beat_the_follower_catches_up_rather_than_re_syncing() {
        // Just under a beat behind: still the same run of indices.
        let t = grid(120.0, 0.0, T0);
        let now = T0 + 0.49;
        let d = due(&t, 0, NO_FLOOR, now, HORIZON, 64);
        assert_eq!(d.first, 0);
        // Just over: re-synced.
        let now = T0 + 0.51;
        let d = due(&t, 0, NO_FLOOR, now, HORIZON, 64);
        assert_eq!(d.first, first_pulse_from(&t, now));
    }

    #[test]
    fn a_new_follower_starts_after_what_is_already_committed() {
        // Another follower has pulses on the wire up to T0 + 0.08: this one
        // begins after them so the ring stays in time order.
        let t = grid(120.0, 0.0, T0);
        let d = due(&t, UNSTARTED, T0 + 0.08, T0, HORIZON, 64);
        assert_eq!(d.first, first_pulse_from(&t, T0 + 0.08));
        assert_eq!(d.first, 4);
        assert!(pulse_ntp(&t, d.first) >= T0 + 0.08);
        // A floor in the past is no floor.
        let d = due(&t, UNSTARTED, T0 - 1.0, T0, HORIZON, 64);
        assert_eq!(d.first, 0);
    }

    #[test]
    fn max_caps_one_call_and_the_rest_are_owed_next_time() {
        let t = grid(120.0, 0.0, T0);
        let d = due(&t, UNSTARTED, NO_FLOOR, T0, HORIZON, 2);
        assert_eq!(d, Due { first: 0, count: 2 });
        let d = due(&t, d.next(), NO_FLOOR, T0, HORIZON, 64);
        assert_eq!(d, Due { first: 2, count: 3 });
        let d = due(&t, d.next(), NO_FLOOR, T0, HORIZON, 0);
        assert_eq!(d, Due { first: 5, count: 0 });
    }

    #[test]
    fn a_grid_anchored_far_from_now_still_indexes_exactly() {
        // The 60 BPM placeholder counts beats from NTP 0: ~9.4e10 pulses by
        // now, well inside i64 and inside f64's exact-integer range.
        let t = Timeline::placeholder(-1);
        let d = due(&t, UNSTARTED, NO_FLOOR, T0, HORIZON, 64);
        assert_eq!(d.first, (T0 * 24.0).ceil() as i64);
        assert_eq!(d.count, 3); // 41.7 ms apart: T0, +41.7, +83.3
        let d2 = due(&t, d.next(), NO_FLOOR, T0 + 0.1, HORIZON, 64);
        assert_eq!(d2.first, d.next());
    }

    #[test]
    fn the_unstarted_sentinel_is_the_header_constant() {
        // cpp/clockwork_clock.h: CLOCKWORK_MIDI_CLOCK_OUT_UNSTARTED is INT64_MIN.
        assert_eq!(UNSTARTED, i64::MIN);
        assert_eq!(PPQN, 24);
    }
}
