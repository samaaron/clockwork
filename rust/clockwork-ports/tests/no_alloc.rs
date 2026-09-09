// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The boundary `clockwork_ports.h` draws, proved rather than asserted in prose:
//! opening a port allocates and is a control-thread call; everything the audio
//! thread and the endpoint touch allocates nothing at all.
//!
//! Clockwork's own `src/rt_alloc.h` cannot prove this. Its hooks replace the
//! C++ global `operator new`/`delete` in the test binary, and Rust's allocator
//! goes to `malloc` directly — a Rust allocation on the audio thread is
//! invisible to it. So the proof has to live on this side, with a counting
//! `#[global_allocator]` and a thread-local arm, and the C++ suite tests what
//! it can see.
//!
//! The arm is thread-local and `const`-initialised, so it cannot itself
//! allocate on first touch, and the two cases here can run on different
//! threads without seeing each other's counts.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::ffi::CString;

use clockwork_ports::ffi::*;

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

const SOURCE: i32 = 1;
const SINK: i32 = 2;

#[test]
fn opening_a_port_allocates_and_that_is_allowed() {
    let name = CString::new("alloc-open").unwrap();   // the caller's, not ours
    arm();
    // SAFETY: a NUL-terminated CString.
    let p = unsafe { clockwork_port_open(name.as_ptr(), SOURCE, 2, 4096) };
    let n = disarm();
    assert_ne!(p, 0);
    assert!(n > 0, "the ring has to come from somewhere");
    clockwork_port_close(p);
}

#[test]
fn nothing_on_the_audio_thread_path_allocates() {
    let src_name = CString::new("alloc-source").unwrap();
    let snk_name = CString::new("alloc-sink").unwrap();
    // SAFETY: a NUL-terminated CString.
    let source = unsafe { clockwork_port_open(src_name.as_ptr(), SOURCE, 2, 1024) };
    // SAFETY: a NUL-terminated CString.
    let sink = unsafe { clockwork_port_open(snk_name.as_ptr(), SINK, 2, 1024) };
    assert!(source != 0 && sink != 0);

    // Everything the calls will touch, built before the arm: the point is
    // whether the port code allocates, not whether the test's own scratch does.
    const N: usize = 128;
    let mut a = vec![0.0f32; N];
    let mut b = vec![0.0f32; N];
    let out_ptrs: [*mut f32; 2] = [a.as_mut_ptr(), b.as_mut_ptr()];
    let in_ptrs: [*const f32; 2] = [a.as_ptr(), b.as_ptr()];
    let mut inter = vec![0.0f32; N * 2];
    let mut slots = [0u32; 8];

    arm();
    for _ in 0..64 {
        // The endpoint's side.
        // SAFETY: `inter` holds `N` frames of 2 channels.
        unsafe { clockwork_port_produce(source, inter.as_ptr(), N as u32) };
        // SAFETY: `inter` holds `N` frames of 2 channels.
        unsafe { clockwork_port_consume(sink, inter.as_mut_ptr(), N as u32) };
        // The audio thread's, including the underrun and overrun paths.
        // SAFETY: two channel pointers to `N` writable frames.
        unsafe { clockwork_port_read(source, out_ptrs.as_ptr(), 2, N as u32) };
        // SAFETY: two channel pointers to `N` writable frames.
        unsafe { clockwork_port_read(source, out_ptrs.as_ptr(), 2, N as u32) };   // now empty
        // SAFETY: two channel pointers to `N` live frames.
        unsafe { clockwork_port_write(sink, in_ptrs.as_ptr(), 2, N as u32) };
        // Reads of a closed / never-valid handle take the same path.
        // SAFETY: as above; the bogus handle is refused by the registry.
        unsafe { clockwork_port_read(0xdead_beef, out_ptrs.as_ptr(), 2, N as u32) };
        // The query surface.
        clockwork_port_writable(source);
        clockwork_port_readable(sink);
        clockwork_port_channels(source);
        clockwork_port_direction(sink);
        clockwork_port_is_open(source);
        clockwork_port_underruns(source);
        clockwork_port_overruns(sink);
        clockwork_port_name(source);
        // SAFETY: `slots` has that many writable entries.
        unsafe { clockwork_port_list(slots.as_mut_ptr(), slots.len() as u32) };
    }
    let n = disarm();
    assert_eq!(n, 0, "the audio-thread path allocated {} time(s)", n);

    clockwork_port_close(source);
    clockwork_port_close(sink);
}
