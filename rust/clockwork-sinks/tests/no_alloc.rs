// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The boundary `src/clockwork_event_sink.h` draws, proved rather than asserted in
//! prose: opening a sink allocates and is a control-thread call; `clockwork_sink_send`
//! allocates nothing at all, on a sink with room and on a sink without.
//!
//! Clockwork's own `src/rt_alloc.h` cannot prove this. Its hooks replace the
//! C++ global `operator new`/`delete` in the test binary, and Rust's allocator
//! goes to `malloc` directly — a Rust allocation on the audio thread is
//! invisible to it. So the proof has to live on this side, with a counting
//! `#[global_allocator]` and a thread-local arm, exactly as
//! `clockwork-ports/tests/no_alloc.rs` does for frames.
//!
//! The arm is thread-local and `const`-initialised, so it cannot itself
//! allocate on first touch, and the drain thread's own allocations — it builds
//! a `Vec` per held message, which is allowed, because it is not the audio
//! thread — are invisible here for the same reason. That is why the sinks
//! below are `Drive::Manual`: not to dodge the drain's allocations, which the
//! arm would not see anyway, but so that nothing empties a queue this test
//! means to fill.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::ffi::CString;
use std::sync::Arc;

use clockwork_sinks::ffi::*;
use clockwork_sinks::registry::{self, Drive};
use clockwork_sinks::profile::Layout as SinkLayout;
use clockwork_sinks::queue::DEFAULT_CELL_BYTES;
use clockwork_sinks::sink::{Stats, KIND_OSC};
use clockwork_sinks::CaptureEndpoint;

thread_local! {
    static ARMED: Cell<bool> = const { Cell::new(false) };
    static COUNT: Cell<usize> = const { Cell::new(0) };
}

struct Counting;

impl Counting {
    #[inline]
    fn note() {
        // `try_with`: during thread teardown the TLS is gone, and an allocator
        // that panicked there would abort the process.
        let _ = ARMED.try_with(|a| {
            if a.get() { let _ = COUNT.try_with(|c| c.set(c.get() + 1)); }
        });
    }
}

// SAFETY: every call is forwarded to `System` unchanged; only a counter is
// added, so `System`'s guarantees are this allocator's.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        Self::note();
        // SAFETY: the caller's contract is `System`'s.
        unsafe { System.alloc(l) }
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        Self::note();
        // SAFETY: as `alloc`.
        unsafe { System.alloc_zeroed(l) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        Self::note();
        // SAFETY: as `alloc`.
        unsafe { System.dealloc(p, l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        Self::note();
        // SAFETY: as `alloc`.
        unsafe { System.realloc(p, l, n) }
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

fn arm() { COUNT.with(|c| c.set(0)); ARMED.with(|a| a.set(true)); }
fn disarm() -> usize { ARMED.with(|a| a.set(false)); COUNT.with(|c| c.get()) }

// One class of ordinary cells, `capacity` deep: the shape every sink had
// before profiles, which is what "full" and "wider than a cell" below mean.
fn manual(capacity: u32) -> (u32, Arc<CaptureEndpoint>) {
    let ep = Arc::new(CaptureEndpoint::new());
    let h = registry::open_with_layout(KIND_OSC, "capture:0",
                                       &SinkLayout::single(DEFAULT_CELL_BYTES as u32, capacity),
                                       ep.clone(), Drive::Manual);
    assert_ne!(h, 0);
    (h, ep)
}

#[test]
fn opening_a_sink_allocates_and_that_is_allowed() {
    let target = CString::new("capture:0").unwrap();   // the caller's, not ours
    let ep: Arc<CaptureEndpoint> = Arc::new(CaptureEndpoint::new());
    arm();
    let h = registry::open_with(KIND_OSC, target.to_str().unwrap(), 1024,
                                ep, Drive::Manual);
    let n = disarm();
    assert_ne!(h, 0);
    assert!(n > 0, "a thousand message cells have to come from somewhere");
    clockwork_sink_close(h);
}

#[test]
fn the_audio_thread_path_allocates_nothing() {
    // Two sinks: one that will always have room, one deliberately kept full,
    // because the full path is a different branch and is the one under
    // pressure when it matters.
    let (roomy, _re) = manual(8192);
    let (full, _fe) = manual(8);
    for i in 0..8u8 {
        // SAFETY: one live byte.
        assert_ne!(unsafe { clockwork_sink_send(full, [i].as_ptr(), 1, 1) }, 0);
    }

    // Everything the calls will touch, built before the arm: the point is
    // whether the sink code allocates, not whether the test's own scratch does.
    let note = [0x90u8, 60, 100];
    let sysex = vec![0xF0u8; 512];
    let oversize = vec![0u8; 4096];
    let mut slots = [0u32; 8];
    let mut stats = Stats::default();
    let now = clockwork_sinks::time::now();
    let future = (now + (1u64 << 32)) as i64;

    arm();
    for _ in 0..256 {
        // The accepted path, immediate and scheduled.
        // SAFETY: `note` is 3 live bytes.
        unsafe { clockwork_sink_send(roomy, note.as_ptr(), 3, 1) };
        // SAFETY: `note` is 3 live bytes.
        unsafe { clockwork_sink_send(roomy, note.as_ptr(), 3, future) };
        // A long message: still a memcpy into a cell taken at open.
        // SAFETY: `sysex` is that many live bytes.
        unsafe { clockwork_sink_send(roomy, sysex.as_ptr(), sysex.len() as u32, 1) };
        // The two refusal paths — queue full, and longer than a cell.
        // SAFETY: `note` is 3 live bytes.
        unsafe { clockwork_sink_send(full, note.as_ptr(), 3, 1) };
        // SAFETY: `oversize` is that many live bytes; the refusal is the point.
        unsafe { clockwork_sink_send(roomy, oversize.as_ptr(), oversize.len() as u32, 1) };
        // A handle that is closed / never existed takes the same path.
        // SAFETY: 3 live bytes; the bogus handle is refused by the registry.
        unsafe { clockwork_sink_send(0xdead_beef, note.as_ptr(), 3, 1) };
        // The query surface, which a metrics tick calls at the same rate.
        clockwork_sink_is_open(roomy);
        clockwork_sink_kind(roomy);
        clockwork_sink_target(roomy);
        // SAFETY: a live out-pointer.
        unsafe { clockwork_sink_stats(roomy, &mut stats) };
        // SAFETY: `slots` has that many writable entries.
        unsafe { clockwork_sink_list(slots.as_mut_ptr(), slots.len() as u32) };
    }
    let n = disarm();
    assert_eq!(n, 0, "the audio-thread path allocated {n} time(s)");

    // And the counters are what the loop implies, so the run above really did
    // take the branches it meant to rather than bailing out early.
    let full_stats = { let mut s = Stats::default();
                       // SAFETY: a live out-pointer.
                       unsafe { clockwork_sink_stats(full, &mut s) }; s };
    assert_eq!(full_stats.dropped, 256, "the full sink refused every send");
    let roomy_stats = { let mut s = Stats::default();
                        // SAFETY: a live out-pointer.
                        unsafe { clockwork_sink_stats(roomy, &mut s) }; s };
    assert_eq!(roomy_stats.dropped, 256, "each oversize message was refused");

    clockwork_sink_close(roomy);
    clockwork_sink_close(full);
}
