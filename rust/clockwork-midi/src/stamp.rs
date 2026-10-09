// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! An input's OS timestamps, on the engine's clock.
//!
//! The OS stamps every MIDI message at the system boundary, and midir hands
//! that stamp over in microseconds — counted from an origin each OS picks:
//! CoreMIDI's host time, the ALSA queue's start, `midiInStart` on WinMM, the
//! port's opening on WinRT. An event carries its moment as an OSC timetag, on
//! the engine's clock (the system clock, as NTP), so the stamp is moved onto
//! it here, at the edge.
//!
//! The offset between the two clocks is the smallest gap seen between the
//! engine's clock when a callback runs and the stamp it was handed. A
//! callback can run late, never early, so the smallest gap is the nearest to
//! the truth. It is the smallest over the last two windows of [`WINDOW_US`],
//! not over all time, so a system clock that is stepped or drifts is followed
//! within two windows rather than never.

use std::time::{Duration, UNIX_EPOCH};

/// How long one window of the smallest-gap search lasts.
pub const WINDOW_US: u64 = 10_000_000;

/// One input's mapping. One per connection: an OS may restart its origin when
/// a port is opened again.
#[derive(Debug, Default)]
pub struct StampClock {
    current: Option<i64>,
    previous: Option<i64>,
    window_start_us: u64,
}

impl StampClock {
    pub fn new() -> Self {
        Self::default()
    }

    /// The timetag of a message the OS stamped `stamp_us`, seen when the
    /// engine's clock read `now_us` (microseconds since the Unix epoch). A
    /// stamp of 0 is no stamp: the message is placed at `now_us`.
    pub fn timetag(&mut self, stamp_us: u64, now_us: u64) -> u64 {
        if stamp_us == 0 {
            return timetag_of_unix_us(now_us);
        }
        if now_us.saturating_sub(self.window_start_us) >= WINDOW_US {
            self.previous = self.current.take();
            self.window_start_us = now_us;
        }
        let gap = now_us as i64 - stamp_us as i64;
        let current = self.current.map_or(gap, |m| m.min(gap));
        self.current = Some(current);
        let offset = self.previous.map_or(current, |p| p.min(current));
        timetag_of_unix_us((stamp_us as i64 + offset).max(0) as u64)
    }
}

/// The engine's clock now, in microseconds since the Unix epoch.
pub fn now_unix_us() -> u64 {
    std::time::SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_micros() as u64)
}

fn timetag_of_unix_us(us: u64) -> u64 {
    clockwork_osc::osc::timetag_of(UNIX_EPOCH + Duration::from_micros(us))
}

#[cfg(test)]
mod tests {
    use super::*;

    const ORIGIN_GAP: u64 = 1_700_000_000_000_000; // the OS's origin, in Unix µs

    #[test]
    fn a_message_is_placed_at_its_stamp_on_the_engine_clock() {
        let mut c = StampClock::new();
        // Callbacks run 300, 0 and 120 µs after the OS stamped their messages.
        c.timetag(1_000, ORIGIN_GAP + 1_000 + 300);
        c.timetag(2_000, ORIGIN_GAP + 2_000);
        let t = c.timetag(3_000, ORIGIN_GAP + 3_000 + 120);
        assert_eq!(t, timetag_of_unix_us(ORIGIN_GAP + 3_000));
    }

    #[test]
    fn a_callback_that_runs_late_does_not_make_its_message_late() {
        let mut c = StampClock::new();
        c.timetag(1_000, ORIGIN_GAP + 1_000);
        let t = c.timetag(5_000, ORIGIN_GAP + 5_000 + 40_000);
        assert_eq!(t, timetag_of_unix_us(ORIGIN_GAP + 5_000));
    }

    #[test]
    fn a_stepped_clock_is_followed_within_two_windows() {
        let mut c = StampClock::new();
        c.timetag(1_000, ORIGIN_GAP + 1_000);
        // The system clock steps forward 5 ms: every gap is now 5 ms wider.
        let step = 5_000;
        let mut t = 0;
        let mut stamp = 1_000;
        while stamp < 1_000 + 2 * WINDOW_US + 1_000_000 {
            stamp += 500_000;
            t = c.timetag(stamp, ORIGIN_GAP + stamp + step);
        }
        assert_eq!(t, timetag_of_unix_us(ORIGIN_GAP + stamp + step));
    }

    #[test]
    fn no_stamp_is_placed_now() {
        let mut c = StampClock::new();
        assert_eq!(c.timetag(0, ORIGIN_GAP), timetag_of_unix_us(ORIGIN_GAP));
    }
}
