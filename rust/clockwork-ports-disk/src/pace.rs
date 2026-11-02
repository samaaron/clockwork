// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! How a disk worker keeps time.
//!
//! A disk stream is paced by time: the audio thread empties a source's ring,
//! and fills a sink's, at the session rate. So a worker wakes every quarter of
//! its ring's duration at that rate and moves everything there is to move. A
//! source then always leaves the audio thread at least three quarters of its
//! ring, and a sink writes a recording to the file within a quarter ring of
//! its arrival. Nothing runs on the audio thread for the worker's sake.
//!
//! The endpoint's close sets the worker's [`Stop`], which ends its sleep at
//! once.

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use clockwork_wait::WaitWord;

/// The shortest wake interval, so a degenerate ring cannot spin its worker.
const MIN_TICK: Duration = Duration::from_micros(100);

/// How long a quarter of a `capacity`-frame ring lasts at `rate` frames a
/// second: the worker's wake interval.
pub fn quarter_ring(capacity: u32, rate: u32) -> Duration {
    Duration::from_secs_f64(f64::from(capacity) / 4.0 / f64::from(rate.max(1))).max(MIN_TICK)
}

/// A worker's stop. [`set`](Stop::set) ends the worker's sleep at once.
#[derive(Debug, Default)]
pub struct Stop {
    set: AtomicBool,
    word: WaitWord,
}

impl Stop {
    pub fn set(&self) {
        self.set.store(true, Ordering::Release);
        self.word.bump();
    }

    pub fn is_set(&self) -> bool {
        self.set.load(Ordering::Acquire)
    }

    /// Sleeps until `deadline`, or for ever with none, unless the stop is set
    /// first. Returns true if it is set.
    pub fn sleep_until(&self, deadline: Option<Instant>) -> bool {
        self.word.wait_for(|| self.is_set(), deadline)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_quarter_ring_is_a_quarter_of_the_ring_at_the_session_rate() {
        assert_eq!(quarter_ring(4096, 48_000), Duration::from_secs_f64(1024.0 / 48_000.0));
        assert_eq!(quarter_ring(16384, 100), Duration::from_secs_f64(40.96));
        // A ring that is gone, or a rate of 0, still sleeps.
        assert_eq!(quarter_ring(0, 48_000), MIN_TICK);
        assert_eq!(quarter_ring(4096, 0), Duration::from_secs_f64(1024.0));
    }

    #[test]
    fn a_stop_ends_a_sleep_at_once_and_a_sleep_without_one_lasts_to_its_deadline() {
        let stop = Stop::default();
        let t0 = Instant::now();
        assert!(!stop.sleep_until(Some(t0 + Duration::from_millis(20))));
        assert!(Instant::now() >= t0 + Duration::from_millis(20));

        std::thread::scope(|s| {
            let sleeper = s.spawn(|| stop.sleep_until(None));
            std::thread::sleep(Duration::from_millis(20));
            stop.set();
            assert!(sleeper.join().unwrap());
        });
        // Set stays set: a later sleep returns at once.
        assert!(stop.sleep_until(None));
    }
}
