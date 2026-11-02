// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Wakes a port's endpoint.
//!
//! An endpoint (the disk reader, the file writer) waits on the audio thread:
//! for frames to take from a sink, or for room to fill in a source. Only the
//! audio thread sees either arrive, a block at a time, and waking the
//! endpoint every block would wake it for nothing most of the time. So the
//! endpoint says how many frames it wants, and the transfer that brings them
//! wakes it, once.
//!
//! Each side writes one thing and then reads the other (the endpoint writes
//! its request and reads the ring; the audio thread writes the ring and reads
//! the request) with a sequentially consistent fence between, so one of them
//! always sees the other. With nobody waiting, the audio thread pays that
//! fence and one load.

#[cfg(not(loom))]
use core::sync::atomic::{fence, AtomicU32, Ordering::{Relaxed, SeqCst}};
#[cfg(loom)]
use loom::sync::atomic::{fence, AtomicU32, Ordering::{Relaxed, SeqCst}};

use clockwork_wait::WaitWord;

/// One endpoint's request, and the word it sleeps on.
#[derive(Debug, Default)]
pub struct EndpointWake {
    word: WaitWord,
    /// Frames the endpoint is waiting for; 0 while it waits for nothing.
    want: AtomicU32,
}

impl EndpointWake {
    #[cfg(not(loom))]
    pub const fn new() -> Self {
        EndpointWake { word: WaitWord::new(), want: AtomicU32::new(0) }
    }

    #[cfg(loom)]
    pub fn new() -> Self {
        EndpointWake::default()
    }

    /// The endpoint blocks until `level()` reaches `frames`, or `stopped()`.
    /// Returns true if the frames are there; false if stopped, or if
    /// `level()` returns `None` because the port has gone. Whoever makes
    /// `stopped()` true then calls [`wake`](Self::wake).
    pub fn wait_for(
        &self, frames: u32, mut level: impl FnMut() -> Option<u32>, mut stopped: impl FnMut() -> bool,
    ) -> bool {
        loop {
            let seen = self.word.value.load(SeqCst);
            if stopped() {
                break;
            }
            match level() {
                None => break,
                Some(l) if l >= frames => return self.withdraw(true),
                Some(_) => {}
            }
            // Ask, then look again with the request in place: a transfer
            // between the look and the ask found nothing asked, so this look
            // must see it.
            self.want.store(frames, SeqCst);
            fence(SeqCst);
            match level() {
                None => break,
                Some(l) if l >= frames => return self.withdraw(true),
                Some(_) => {}
            }
            self.word.wait(seen);
        }
        self.withdraw(false)
    }

    /// The audio thread calls this after a transfer that leaves `level`
    /// frames to take (a sink) or room for that many (a source).
    #[inline]
    pub fn answer(&self, level: u32) {
        fence(SeqCst);
        let want = self.want.load(Relaxed);
        if want != 0 && level >= want && self.want.swap(0, SeqCst) != 0 {
            self.word.bump();
        }
    }

    /// Wakes the endpoint whatever it waits for: for a stop, or the port
    /// closing.
    pub fn wake(&self) {
        self.word.bump();
    }

    fn withdraw(&self, got: bool) -> bool {
        self.want.store(0, SeqCst);
        got
    }
}

#[cfg(all(test, not(loom)))]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicBool;

    #[test]
    fn an_endpoint_that_has_its_frames_does_not_wait_and_asks_for_nothing() {
        let w = EndpointWake::new();
        assert!(w.wait_for(64, || Some(64), || false));
        assert_eq!(w.want.load(SeqCst), 0);
    }

    #[test]
    fn a_transfer_short_of_the_request_wakes_nobody_and_one_that_meets_it_wakes_the_endpoint_once() {
        let w = EndpointWake::new();
        w.want.store(64, SeqCst);
        let before = w.word.value.load(SeqCst);
        w.answer(63);
        assert_eq!(w.word.value.load(SeqCst), before);
        w.answer(64);
        assert_eq!(w.word.value.load(SeqCst), before + 1);
        w.answer(128);   // taken already
        assert_eq!(w.word.value.load(SeqCst), before + 1);
    }

    #[test]
    fn a_stop_or_a_closed_port_ends_the_wait() {
        let w = EndpointWake::new();
        let stop = AtomicBool::new(false);
        std::thread::scope(|s| {
            let waiter = s.spawn(|| w.wait_for(64, || Some(0), || stop.load(SeqCst)));
            while w.word.waiters.load(SeqCst) == 0 { std::thread::yield_now(); }
            stop.store(true, SeqCst);
            w.wake();
            assert!(!waiter.join().unwrap());
        });
        assert!(!w.wait_for(64, || None, || false));
        assert_eq!(w.want.load(SeqCst), 0);
    }
}

// Explores every interleaving of an endpoint asking and the audio thread
// transferring, on loom's atomics. A wake lost between the endpoint's look
// and its sleep leaves it blocked with its frames there, which loom reports
// as a deadlock. Run with: RUSTFLAGS="--cfg loom" cargo test --release
// -p clockwork-ports --lib wake.
#[cfg(all(test, loom))]
mod loom_tests {
    use super::*;
    use loom::sync::Arc;
    use loom::thread;

    #[test]
    fn a_transfer_that_brings_the_frames_always_wakes_the_endpoint() {
        loom::model(|| {
            let w = Arc::new(EndpointWake::new());
            let level = Arc::new(AtomicU32::new(0));
            let audio = {
                let (w, level) = (w.clone(), level.clone());
                thread::spawn(move || {
                    level.store(64, SeqCst);   // the transfer
                    w.answer(64);
                })
            };
            assert!(w.wait_for(64, || Some(level.load(SeqCst)), || false));
            audio.join().unwrap();
        });
    }

    #[test]
    fn a_stop_always_ends_the_wait() {
        loom::model(|| {
            let w = Arc::new(EndpointWake::new());
            let stop = Arc::new(loom::sync::atomic::AtomicBool::new(false));
            let stopper = {
                let (w, stop) = (w.clone(), stop.clone());
                thread::spawn(move || {
                    stop.store(true, SeqCst);
                    w.wake();
                })
            };
            assert!(!w.wait_for(64, || Some(0), || stop.load(SeqCst)));
            stopper.join().unwrap();
        });
    }
}
