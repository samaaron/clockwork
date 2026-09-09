// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The one thread that drains every sink.
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
//! It starts on the first sink opened and stops when the last one closes.
//! Nothing runs in a process that opened no sink.
//!
//! # Why it polls, and what that costs
//!
//! It cannot be woken by a send. `clockwork_sink_send` is the audio thread's, and
//! signalling a condition variable or unparking a thread is a syscall — the
//! header forbids exactly that. So the drain looks on a short cycle instead,
//! and the cost is honest: an "immediately" message waits up to [`TICK`]
//! before it goes out.
//!
//! Which is the strongest possible argument for `when`. A DSP that says WHEN
//! IT MEANS pays none of this: the message is picked up on some earlier tick,
//! held, and released at its time — the poll interval becomes scheduling slack
//! instead of latency. The 500 µs is only ever paid by a producer that decided
//! at the last possible moment, and that producer had already decided to be
//! late.
//!
//! When a message IS being held, the sleep shortens to land on it rather than
//! waiting out the tick, so the release is limited by the OS timer's own
//! resolution rather than by this loop.

use std::sync::Mutex;

use crate::registry;
use crate::time;

/// The longest the drain sleeps while any sink is open. See the module note:
/// this is the latency of "immediately", and nothing else.
pub const TICK: std::time::Duration = std::time::Duration::from_micros(500);

/// The shortest. A held message due in 20 µs is not worth a wake per 20 µs;
/// the drain lands on it a hair early or a hair late either way.
const MIN_SLEEP: std::time::Duration = std::time::Duration::from_micros(100);

/// The lifecycle, under a lock rather than a flag.
///
/// A flag plus a compare-exchange has a window where the thread has decided to
/// stop and an `open` has decided it is already running, and the result is a
/// sink nobody drains. This is not a hot path — it is touched on open and on
/// the tick where the last sink went away — so the lock costs nothing and the
/// window does not exist.
static RUNNING: Mutex<bool> = Mutex::new(false);

/// Start the drain if it is not already running. Called by every
/// `Drive::Service` open.
pub fn ensure_running() {
    let Ok(mut running) = RUNNING.lock() else { return };
    if *running { return; }
    match std::thread::Builder::new().name("clockwork-sink-drain".into()).spawn(run) {
        Ok(_) => *running = true,
        // No thread. Sinks still accept and count; nothing is delivered, and
        // the numbers say so rather than the process pretending.
        Err(e) => eprintln!("[sinks] could not start the drain thread: {e}"),
    }
}

fn run() {
    loop {
        let now = time::now();
        let next = registry::pump_service(now);

        if registry::open_count() == 0 {
            // Under the lock, so an `open` cannot decide we are still running
            // while we are deciding to stop. Re-checked inside it, because a
            // sink may have opened between the count above and the lock.
            if let Ok(mut running) = RUNNING.lock() {
                if registry::open_count() == 0 {
                    *running = false;
                    return;
                }
            }
        }

        let sleep = next
            .and_then(|w| time::until(w, now))
            .unwrap_or(TICK)
            .clamp(MIN_SLEEP, TICK);
        std::thread::sleep(sleep);
    }
}
