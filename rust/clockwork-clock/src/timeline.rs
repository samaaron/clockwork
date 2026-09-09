// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The timeline snapshot: one beat grid, in NTP seconds.
//!
//! A [`Timeline`] is a value, not a handle: a tempo, one (beat, time) anchor
//! and the transport, copied out of whatever source owns the grid at the
//! moment of asking. Everything a reader wants is a pure function of the
//! copy, so a reader never holds a lock and never sees a half-written grid.
//!
//! The same struct crosses the C ABI (`cpp/clockwork_clock.h`: `ClockworkTimeline`),
//! hence `repr(C)`, the i32 booleans, and the pinned layout test.

/// One beat grid. Times are NTP seconds (since 1900); beats are quarter notes.
///
/// `align(8)` is not decoration: the members are 52 bytes, and a 64-bit ABI
/// rounds that to 56 while a 32-bit one leaves it at 52. Both the C++ size
/// assertion and the cross-process mirror (which carries this as whole 64-bit
/// words) require 56, so the alignment is stated on both sides rather than
/// inherited from whichever ABI happens to be compiling.
#[repr(C, align(8))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Timeline {
    /// Beats per minute. Always >= 1.
    pub bpm: f64,
    /// The anchor: `anchor_beat` was (or will be) at `anchor_ntp`. Every other
    /// beat lies `bpm / 60` per second either side of it.
    pub anchor_beat: f64,
    pub anchor_ntp: f64,
    /// When the transport last changed state, or 0 if it never has. Preserved
    /// as a sentinel rather than converted, because 0 converted is a
    /// plausible-looking time in 1900.
    pub transition_ntp: f64,
    /// Which timeline this is: 0 = Link, 1.. = the midi slots, -1 = a never
    /// seen name answered by [`Timeline::placeholder`].
    pub id: i32,
    /// Transport running (1) or stopped (0).
    pub playing: i32,
    /// Whether anything has defined where beat 0 is. Link's grid always exists;
    /// a MIDI follower's does once it has seen a Start or a Song Position.
    /// Without one, bar phase means nothing.
    pub anchored: i32,
    /// Meter, as `meter_num` / `meter_den`: how quarter-note beats group
    /// into bars (`beats_per_bar`). 4/4 until a source sets it — the
    /// `/clockwork/clock/[<tl>/]meter` verb for Link and the midi slots; no
    /// MIDI message carries one.
    pub meter_num: i32,
    pub meter_den: i32,
}

/// Whether `num`/`den` is a meter a bar can be built from: at least one
/// beat per bar, over a power-of-two note value up to a 32nd.
pub fn is_valid_meter(num: i32, den: i32) -> bool {
    num >= 1 && matches!(den, 1 | 2 | 4 | 8 | 16 | 32)
}

impl Timeline {
    /// The tempo a never-seen timeline free-runs at.
    pub const FALLBACK_BPM: f64 = 60.0;

    /// A timeline nobody has fed: 60 BPM from the NTP epoch, stopped,
    /// unanchored. What a client asking about an unclaimed port is told.
    pub const fn placeholder(id: i32) -> Self {
        Self {
            bpm: Self::FALLBACK_BPM,
            anchor_beat: 0.0,
            anchor_ntp: 0.0,
            transition_ntp: 0.0,
            id,
            playing: 0,
            anchored: 0,
            meter_num: 4,
            meter_den: 4,
        }
    }

    /// The beat at NTP time `ntp`.
    pub fn beat_at(&self, ntp: f64) -> f64 {
        self.anchor_beat + (ntp - self.anchor_ntp) * self.bpm / 60.0
    }

    /// The NTP time of `beat`.
    pub fn time_at_beat(&self, beat: f64) -> f64 {
        self.anchor_ntp + (beat - self.anchor_beat) * 60.0 / self.bpm
    }

    /// The phase of the beat at `ntp` within `quantum` beats.
    pub fn phase_at(&self, ntp: f64, quantum: f64) -> f64 {
        wrap_phase(self.beat_at(ntp), quantum)
    }

    /// Quarter-note beats in one bar of this meter: 4/4 is 4, 7/8 is 3.5,
    /// 3/2 is 6. Beats stay quarter notes whatever the meter, because that
    /// is the unit every tempo, every anchor and every plugin host speaks;
    /// the meter only says where the bar lines fall.
    pub fn beats_per_bar(&self) -> f64 {
        self.meter_num as f64 * 4.0 / self.meter_den as f64
    }

    /// The bar at `ntp`, 0-based, counted from beat 0 (bar 0 starts there).
    /// Whole-valued; negative before beat 0.
    pub fn bar_at(&self, ntp: f64) -> f64 {
        (self.beat_at(ntp) / self.beats_per_bar()).floor()
    }

    /// Quarter-note beats since the start of the bar at `ntp`: in
    /// `0..beats_per_bar()`.
    pub fn beat_in_bar_at(&self, ntp: f64) -> f64 {
        wrap_phase(self.beat_at(ntp), self.beats_per_bar())
    }
}

/// Non-negative phase of `beat` within `quantum`; 0 when there is no quantum.
/// Twin of `clockwork::wrapPhase` (C++) and `wrapPhase` (JS).
pub fn wrap_phase(beat: f64, quantum: f64) -> f64 {
    if quantum <= 0.0 {
        return 0.0;
    }
    let p = beat % quantum;
    if p < 0.0 {
        p + quantum
    } else {
        p
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn grid(bpm: f64, anchor_beat: f64, anchor_ntp: f64) -> Timeline {
        Timeline {
            bpm,
            anchor_beat,
            anchor_ntp,
            ..Timeline::placeholder(0)
        }
    }

    #[test]
    fn beat_advances_from_the_anchor_at_the_tempo() {
        let t = grid(120.0, 8.0, 1000.0);
        assert_eq!(t.beat_at(1000.0), 8.0);
        assert_eq!(t.beat_at(1001.0), 10.0);
        assert_eq!(t.beat_at(999.5), 7.0);
    }

    #[test]
    fn time_at_beat_inverts_beat_at() {
        let t = grid(93.0, 3.25, 4_000_000_000.5);
        for b in [0.0, 3.25, 17.0, -2.0, 1e6] {
            let at = t.time_at_beat(b);
            assert!((t.beat_at(at) - b).abs() < 1e-6, "beat {b}");
        }
    }

    #[test]
    fn phase_wraps_non_negative_within_the_quantum() {
        let t = grid(60.0, 0.0, 0.0);
        assert_eq!(t.phase_at(5.5, 4.0), 1.5);
        assert_eq!(t.phase_at(-0.5, 4.0), 3.5);
        assert_eq!(t.phase_at(5.5, 0.0), 0.0); // no quantum: no phase
    }

    #[test]
    fn bars_group_quarter_note_beats_by_the_meter() {
        let mut t = grid(120.0, 0.0, 1000.0); // 2 beats/s from beat 0 at 1000
        assert_eq!(t.beats_per_bar(), 4.0);
        assert_eq!(t.bar_at(1000.0), 0.0);
        assert_eq!(t.bar_at(1001.5), 0.0); // beat 3
        assert_eq!(t.bar_at(1002.0), 1.0); // beat 4
        assert_eq!(t.beat_in_bar_at(1002.75), 1.5); // beat 5.5
        assert_eq!(t.bar_at(999.0), -1.0); // beat -2: before the origin
        assert_eq!(t.beat_in_bar_at(999.0), 2.0);

        t.meter_num = 7;
        t.meter_den = 8;
        assert_eq!(t.beats_per_bar(), 3.5);
        assert_eq!(t.bar_at(1010.0), 5.0); // beat 20: bars at 0, 3.5, .. 17.5
        assert_eq!(t.beat_in_bar_at(1010.0), 2.5);

        t.meter_num = 3;
        t.meter_den = 2;
        assert_eq!(t.beats_per_bar(), 6.0);
        assert_eq!(t.bar_at(1003.0), 1.0); // beat 6
        assert_eq!(t.beat_in_bar_at(1003.0), 0.0);
    }

    #[test]
    fn a_meter_is_a_count_over_a_power_of_two_note() {
        for den in [1, 2, 4, 8, 16, 32] {
            assert!(is_valid_meter(1, den), "1/{den}");
            assert!(is_valid_meter(13, den), "13/{den}");
        }
        assert!(!is_valid_meter(0, 4));
        assert!(!is_valid_meter(-3, 4));
        assert!(!is_valid_meter(4, 0));
        assert!(!is_valid_meter(4, 3));
        assert!(!is_valid_meter(4, 64));
        assert!(!is_valid_meter(4, -4));
    }

    #[test]
    fn placeholder_is_a_never_seen_timeline() {
        // 60 BPM free-run from the epoch: what a client asking about an
        // unclaimed port has always been told.
        let t = Timeline::placeholder(-1);
        assert_eq!(t.id, -1);
        assert_eq!(t.bpm, Timeline::FALLBACK_BPM);
        assert_eq!(t.beat_at(90.0), 90.0);
        assert_eq!(t.playing, 0);
        assert_eq!(t.anchored, 0);
        assert_eq!(t.transition_ntp, 0.0);
        assert_eq!((t.meter_num, t.meter_den), (4, 4));
    }

    #[test]
    fn layout_is_the_c_struct() {
        // cpp/clockwork_clock.h mirrors this field for field; both sides pin the size.
        assert_eq!(std::mem::size_of::<Timeline>(), 56);
        assert_eq!(std::mem::align_of::<Timeline>(), 8);
    }

    #[test]
    fn wrap_phase_matches_the_cpp_twin() {
        assert_eq!(wrap_phase(9.0, 4.0), 1.0);
        assert_eq!(wrap_phase(-1.0, 4.0), 3.0);
        assert_eq!(wrap_phase(9.0, -1.0), 0.0);
    }
}
