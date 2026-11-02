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
//! # How it sleeps
//!
//! The drain thread sleeps on [`DRAIN`], a `clockwork_wait::Doorbell`, until a
//! send queues a message, the last sink closes, or the earliest held message
//! is due. `clockwork_sink_send` rings the doorbell after it queues. While the
//! drain is awake a ring costs the audio thread a fence and a load. The first
//! ring after the drain goes to sleep makes one wake call, which does not
//! block. An "immediately" message goes out as soon as the OS schedules the
//! drain thread, and a timed message goes out at its time, to the resolution
//! of the OS timer.

use std::sync::Mutex;
use std::time::Instant;

use clockwork_wait::Doorbell;

use crate::registry;
use crate::time;

/// Sends and closes ring this, and the drain thread sleeps on it.
pub static DRAIN: Doorbell = Doorbell::new();

/// The shortest sleep. A held message due in 20 µs is not worth a wake per
/// 20 µs; the drain lands on it a hair early or a hair late either way.
const MIN_SLEEP: std::time::Duration = std::time::Duration::from_micros(100);

/// The lifecycle, under a lock rather than a flag.
///
/// A flag plus a compare-exchange has a window where the thread has decided to
/// stop and an `open` has decided it is already running, and the result is a
/// sink nobody drains. This is not a hot path — it is touched on open and on
/// the pass where the last sink went away — so the lock costs nothing and the
/// window does not exist.
static RUNNING: Mutex<bool> = Mutex::new(false);

/// Start the drain if it is not already running. Called by every
/// `Drive::Service` open.
pub fn ensure_running() {
    let Ok(mut running) = RUNNING.lock() else { return };
    if *running { return; }
    match std::thread::Builder::new().name("cw-sink-drain".into()).spawn(run) {
        Ok(_) => *running = true,
        // No thread. Sinks still accept and count; nothing is delivered, and
        // the numbers say so rather than the process pretending.
        Err(e) => clockwork_log::log!("[sinks] could not start the drain thread: {e}"),
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

        // Sleep until a send queues a message, the last sink closes, or the
        // earliest held message is due.
        let deadline = next.map(|w| Instant::now() + time::until(w, now).unwrap_or_default().max(MIN_SLEEP));
        DRAIN.sleep(|| registry::any_queued() || registry::open_count() == 0, deadline);
    }
}
