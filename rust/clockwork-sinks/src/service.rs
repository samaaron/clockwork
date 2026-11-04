// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The one thread that drains every sink, of every table.
//!
//! # Why the crate owns a thread at all
//!
//! `src/clockwork_event_sink.h` has no drain call. A caller opens a sink, sends to
//! it, and reads its stats; nothing in that surface says "and now do the
//! work", and adding one would put the timing of every outgoing message into
//! the hands of whoever remembered to call it. So the substrate owns the
//! thread, exactly as `clockwork-ports-disk` owns one per file — one for the
//! lot here, because a sink's work is a few microseconds and sixty-four
//! threads asleep is worse than one awake.
//!
//! It starts on the first sink opened and stops when the last one closes,
//! whichever table — whichever core — they are in. Nothing runs in a process
//! that opened no sink.
//!
//! # How it sleeps
//!
//! The drain thread sleeps on [`DRAIN`], a `clockwork_wait::Doorbell`, until a
//! send queues a message, the last sink closes, or the earliest held message
//! is due, and for at most [`WALL_CLOCK_FOLLOW`] while it holds one. `clockwork_sink_send` rings the doorbell after it queues. While the
//! drain is awake a ring costs the audio thread a fence and a load. The first
//! ring after the drain goes to sleep makes one wake call, which does not
//! block. An "immediately" message goes out as soon as the OS schedules the
//! drain thread, and a timed message goes out at its time, to the resolution
//! of the OS timer.

use std::sync::{Arc, Mutex, Weak};
use std::time::Instant;

use clockwork_log::CoreRef;
use clockwork_wait::Doorbell;

use crate::registry::Sinks;
use crate::time;
use clockwork_osc::Timetag;

/// Sends and closes ring this, and the drain thread sleeps on it.
pub static DRAIN: Doorbell = Doorbell::new();

/// The shortest sleep. A held message due in 20 µs is not worth a wake per
/// 20 µs; the drain lands on it a hair early or a hair late either way.
const MIN_SLEEP: std::time::Duration = std::time::Duration::from_micros(100);

/// The longest the drain sleeps while it holds a message. A timetag is on the
/// wall clock, which a user or an NTP sync can set, and which moves on while a
/// laptop sleeps, but the drain sleeps on the steady clock. This is how soon
/// it sees the wall clock move. The scheduler host has the same bound
/// (`kWallClockFollow`, src/host/host_scheduler.h).
const WALL_CLOCK_FOLLOW: std::time::Duration = std::time::Duration::from_secs(1);

/// Every table made ([`Sinks::new`]) and not yet gone: what the drain walks.
static TABLES: Mutex<Vec<Weak<Sinks>>> = Mutex::new(Vec::new());

/// A new table, for the drain to walk from now on.
pub(crate) fn register(table: &Arc<Sinks>) {
    if let Ok(mut tables) = TABLES.lock() {
        tables.retain(|t| t.strong_count() > 0);
        tables.push(Arc::downgrade(table));
    }
}

/// The tables still live, held for one pass.
fn live() -> Vec<Arc<Sinks>> {
    TABLES.lock().map(|tables| tables.iter().filter_map(Weak::upgrade).collect()).unwrap_or_default()
}

/// Sinks open, every table together.
fn open_count() -> u32 {
    live().iter().map(|t| t.open_count()).sum()
}

/// The lifecycle, under a lock rather than a flag.
///
/// A flag plus a compare-exchange has a window where the thread has decided to
/// stop and an `open` has decided it is already running, and the result is a
/// sink nobody drains. This is not a hot path — it is touched on open and on
/// the pass where the last sink went away — so the lock costs nothing and the
/// window does not exist.
static RUNNING: Mutex<bool> = Mutex::new(false);

/// Start the drain if it is not already running. Called by every
/// `Drive::Service` open; `core` is the opener's, told when it cannot.
pub fn ensure_running(core: CoreRef) {
    let Ok(mut running) = RUNNING.lock() else { return };
    if *running { return; }
    match std::thread::Builder::new().name("cw-sink-drain".into()).spawn(run) {
        Ok(_) => *running = true,
        // No thread. Sinks still accept and count; nothing is delivered, and
        // the numbers say so rather than the process pretending.
        Err(e) => clockwork_log::log!(core.ptr(), "[sinks] could not start the drain thread: {e}"),
    }
}

fn run() {
    loop {
        let now = time::now();
        let next = live().iter().filter_map(|t| t.pump_service(now)).min();

        if open_count() == 0 {
            // Under the lock, so an `open` cannot decide we are still running
            // while we are deciding to stop. Re-checked inside it, because a
            // sink may have opened between the count above and the lock.
            if let Ok(mut running) = RUNNING.lock() {
                if open_count() == 0 {
                    *running = false;
                    return;
                }
            }
        }

        // Sleep until a send queues a message, the last sink closes, or the
        // earliest held message is due. Measured from the clock after the
        // pump, which may have spent a while delivering.
        let deadline = sleep_for(next, time::now()).map(|d| Instant::now() + d);
        DRAIN.sleep(|| { let t = live(); t.iter().any(|t| t.any_queued()) || t.iter().all(|t| t.open_count() == 0) }, deadline);
    }
}

/// How long the drain sleeps towards `next`, when it must next pump, as seen
/// at `now`.
fn sleep_for(next: Option<Timetag>, now: Timetag) -> Option<std::time::Duration> {
    next.map(|w| time::until(w, now).unwrap_or_default().clamp(MIN_SLEEP, WALL_CLOCK_FOLLOW))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn a_drain_holding_a_message_sleeps_a_second_at_most_so_it_sees_the_wall_clock_move() {
        // The sleep runs on the steady clock and the timetag is on the wall
        // clock, which can be set, or move on while a laptop sleeps.
        let now = time::now();
        let an_hour_on = now.plus(3600i64 << 32);
        assert_eq!(sleep_for(Some(an_hour_on), now), Some(Duration::from_secs(1)));

        let soon = now.plus((1i64 << 32) / 10);   // 100 ms
        let d = sleep_for(Some(soon), now).unwrap();
        assert!(d > Duration::from_millis(99) && d <= Duration::from_millis(100), "{d:?}");
        assert_eq!(sleep_for(Some(now), now), Some(MIN_SLEEP));   // due: the shortest sleep
        assert_eq!(sleep_for(None, now), None);                   // nothing held: until a send
    }
}
