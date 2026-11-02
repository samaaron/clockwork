// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `src/clockwork_clock_state.h`: the session clock as a guest reads it
//! through `DspConfig::clock`, and [`read`], the Rust twin of the header's
//! `readClockworkClock()`.
//!
//! The state is a C++ struct of atomics, so the C probe cannot see it; the
//! header pins every offset with a `static_assert` and the constants below
//! assert the same numbers, so the two cannot drift apart unnoticed.
//!
//! There is no seqlock. Writers store the value first and the field that
//! keys it last, with release (bpm keys the beat origin; is_playing keys its
//! timestamp), and bump `generation` after the grid moves. [`read`] loads in
//! the order that makes that protocol hold — the same order as the C++
//! reader — so no guest has to reproduce it.

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

/// Session tempo before anyone sets one (the header's `kDefaultBpm`).
pub const DEFAULT_BPM: f64 = 120.0;

/// `ClockworkClockState`. Written by clockwork, read by everyone else.
#[repr(C, align(8))]
pub struct ClockworkClockState {
    /// BPM as an IEEE 754 bit pattern.
    pub bpm: AtomicU64,
    /// NTP seconds at which beat 0 occurred, as a bit pattern.
    pub beat_origin_ntp: AtomicU64,
    /// NTP seconds the transport last changed, as a bit pattern.
    pub is_playing_at_ntp: AtomicU64,
    pub is_playing: AtomicU32,
    pub flags: AtomicU32,
    /// `(num << 16) | den`.
    pub meter: AtomicU32,
    /// One more each time the grid (tempo, origin) moves.
    pub generation: AtomicU32,
}

const _: () = {
    assert!(core::mem::size_of::<ClockworkClockState>() == 40);
    assert!(core::mem::offset_of!(ClockworkClockState, bpm) == 0);
    assert!(core::mem::offset_of!(ClockworkClockState, beat_origin_ntp) == 8);
    assert!(core::mem::offset_of!(ClockworkClockState, is_playing_at_ntp) == 16);
    assert!(core::mem::offset_of!(ClockworkClockState, is_playing) == 24);
    assert!(core::mem::offset_of!(ClockworkClockState, flags) == 28);
    assert!(core::mem::offset_of!(ClockworkClockState, meter) == 32);
    assert!(core::mem::offset_of!(ClockworkClockState, generation) == 36);
};

/// One coherent reading of the clock (`ClockworkClockSnapshot`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ClockSnapshot {
    pub bpm: f64,
    pub beat_origin_ntp: f64,
    pub is_playing_at_ntp: f64,
    pub is_playing: bool,
    pub flags: u32,
    pub meter_num: i32,
    pub meter_den: i32,
    /// The grid's generation when this was read: a follower keeping its own
    /// copy of the grid re-reads when it has moved on.
    pub generation: u32,
}

impl ClockSnapshot {
    /// What a guest sees when the host gave it no clock: the default tempo,
    /// 4/4, beat 0 at NTP 0, stopped.
    pub const ABSENT: ClockSnapshot = ClockSnapshot {
        bpm: DEFAULT_BPM,
        beat_origin_ntp: 0.0,
        is_playing_at_ntp: 0.0,
        is_playing: false,
        flags: 0,
        meter_num: 4,
        meter_den: 4,
        generation: 0,
    };

    /// The beat at an NTP instant (seconds), under this snapshot.
    pub fn beat_at(&self, ntp_seconds: f64) -> f64 {
        (ntp_seconds - self.beat_origin_ntp) * self.bpm / 60.0
    }

    /// The NTP instant (seconds) of a beat, under this snapshot.
    pub fn time_at_beat(&self, beat: f64) -> f64 {
        self.beat_origin_ntp + beat * 60.0 / self.bpm
    }
}

/// Read the clock whole. `None` (the host offered no clock) reads as
/// [`ClockSnapshot::ABSENT`].
pub fn read(state: Option<&ClockworkClockState>) -> ClockSnapshot {
    let Some(s) = state else { return ClockSnapshot::ABSENT };
    // The generation before the grid: writers bump it after, so the grid read
    // is never older than the generation says.
    let generation = s.generation.load(Ordering::Acquire);
    // bpm keys beat_origin_ntp; is_playing keys its timestamp. Acquire the
    // key, then the value it anchors.
    let bpm = f64::from_bits(s.bpm.load(Ordering::Acquire));
    let beat_origin_ntp = f64::from_bits(s.beat_origin_ntp.load(Ordering::Relaxed));
    let is_playing = s.is_playing.load(Ordering::Acquire) != 0;
    let is_playing_at_ntp = f64::from_bits(s.is_playing_at_ntp.load(Ordering::Relaxed));
    let flags = s.flags.load(Ordering::Relaxed);
    let meter = s.meter.load(Ordering::Relaxed);
    ClockSnapshot {
        bpm,
        beat_origin_ntp,
        is_playing_at_ntp,
        is_playing,
        flags,
        meter_num: (meter >> 16) as i32,
        meter_den: (meter & 0xFFFF) as u16 as i16 as i32,
        generation,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn state(bpm: f64, origin: f64) -> ClockworkClockState {
        ClockworkClockState {
            bpm: AtomicU64::new(bpm.to_bits()),
            beat_origin_ntp: AtomicU64::new(origin.to_bits()),
            is_playing_at_ntp: AtomicU64::new(0),
            is_playing: AtomicU32::new(1),
            flags: AtomicU32::new(0),
            meter: AtomicU32::new((3 << 16) | 4),
            generation: AtomicU32::new(7),
        }
    }

    #[test]
    fn reads_the_grid_and_places_beats_on_it() {
        let s = state(90.0, 1000.0);
        let c = read(Some(&s));
        assert_eq!((c.bpm, c.beat_origin_ntp, c.generation), (90.0, 1000.0, 7));
        assert_eq!((c.meter_num, c.meter_den, c.is_playing), (3, 4, true));
        assert_eq!(c.beat_at(1002.0), 3.0);
        assert_eq!(c.time_at_beat(3.0), 1002.0);
    }

    #[test]
    fn no_clock_reads_as_the_default() {
        assert_eq!(read(None), ClockSnapshot::ABSENT);
    }
}
