// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The pool's defining property: allocation cost does not depend on how much
//! is already in it.
//!
//! This is a guard, not a benchmark. Before the size-class index, `alloc` was
//! a first-fit walk over every block from the area start and `free` walked the
//! whole area to coalesce, so both were linear in pool occupancy. On the audio
//! thread that is not a slow allocator, it is a broken one: a few hundred live
//! synths took allocation from nanoseconds to hundreds of microseconds, the
//! block deadline went with it, and the engine stopped dead rather than
//! degrading.
//!
//! The pool is constant-time by construction — two bitmap scans, no walk —
//! so there is no loop to count: only a measurement of the cost can catch a
//! walk someone adds later. What is measured is the thread's own CPU time,
//! not the wall clock: time the thread spends preempted or waiting is not in
//! it, so a busy machine cannot make the pool look slower than it is, and
//! only work the pool does can. The regression it guards against was 90x to
//! 170x, against ~1x for the pool as it is.
//!
//! The pool's code is the same for every target, wasm included, so measuring
//! it natively — where a thread CPU clock exists — covers the AudioWorklet's
//! allocator too.
#![cfg(any(unix, windows))]

use std::ffi::c_void;
use clockwork_heap::HeapPool;

/// CPU time this thread has used: nanoseconds on Unix, cycles on Windows.
/// Only ratios of it are compared, so the unit cancels.
#[cfg(unix)]
fn thread_cpu() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: a valid out-pointer to a timespec.
    let rc = unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut ts) };
    assert_eq!(rc, 0, "no thread CPU clock");
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

#[cfg(windows)]
fn thread_cpu() -> u64 {
    use windows_sys::Win32::System::Threading::GetCurrentThread;
    use windows_sys::Win32::System::WindowsProgramming::QueryThreadCycleTime;
    let mut cycles = 0u64;
    // SAFETY: GetCurrentThread is a pseudo-handle for this thread, always
    // valid; `cycles` is a valid out-pointer.
    let ok = unsafe { QueryThreadCycleTime(GetCurrentThread(), &mut cycles) };
    assert_ne!(ok, 0, "no thread cycle count");
    cycles
}

unsafe extern "C" fn new_area(size: usize) -> *mut c_void {
    let l = std::alloc::Layout::from_size_align(size, 16).unwrap();
    // SAFETY: the pool never asks for zero bytes.
    unsafe { std::alloc::alloc(l) }.cast::<c_void>()
}
unsafe extern "C" fn free_area(_p: *mut c_void) {}

fn pool() -> HeapPool {
    HeapPool::new(Some(new_area), Some(free_area), 48 * 1024 * 1024, 0)
}

/// The thread's CPU time per alloc/free, best of five passes: what is left
/// of the machine in it (another process warming the shared cache, a timer
/// interrupt charged to the thread) is noise of a few percent, and the least
/// of five is the pool's own cost.
fn cpu_per_op(setup: impl Fn(&mut HeapPool) -> Vec<*mut u8>, size: usize) -> f64 {
    let mut best = f64::MAX;
    for _ in 0..5 {
        let mut p = pool();
        let held = setup(&mut p);
        // Long enough to stand well clear of the clock's resolution.
        let churn = 20_000;
        let t = thread_cpu();
        for _ in 0..churn {
            let q = p.alloc(size);
            assert!(!q.is_null());
            // SAFETY: just allocated from this pool, freed once.
            unsafe { p.free(q) };
        }
        let per_op = (thread_cpu() - t) as f64 / (churn as f64 * 2.0);
        best = best.min(per_op);
        for h in held {
            // SAFETY: each was allocated by `setup` from this pool.
            unsafe { p.free(h) };
        }
    }
    best
}

/// TLSF measures 1.0x across these ranges on the thread's CPU clock (0.99x
/// and 1.00x on an M-series Mac), and the linear walk it replaced measured
/// 90x and 170x. Measured on a clock load cannot inflate, the bound can sit
/// close enough to catch a walk over a fraction of the pool, and still far
/// from anything a cache's worth of difference between machines makes.
const MAX_RATIO: f64 = 5.0;

#[test]
fn cost_is_flat_in_live_blocks() {
    let small = cpu_per_op(|p| (0..500).map(|_| p.alloc(256)).collect(), 256);
    let large = cpu_per_op(|p| (0..32_000).map(|_| p.alloc(256)).collect(), 256);
    assert!(
        large < small * MAX_RATIO,
        "allocation cost grew with occupancy: {small:.1} at 500 live blocks, \
         {large:.1} at 32000 (thread CPU per op) — the pool is scanning what it holds"
    );
}

#[test]
fn cost_is_flat_in_free_blocks() {
    // Assorted sizes, alternately kept and freed, so the holes cannot
    // coalesce with one another: this is the shape a single free list cannot
    // survive, because first-fit has to inspect every one of them.
    let fragment = |n: usize| {
        move |p: &mut HeapPool| {
            let mut held = Vec::with_capacity(n);
            let mut holes = Vec::with_capacity(n);
            for i in 0..n {
                held.push(p.alloc(64 + (i % 32) * 16));
                holes.push(p.alloc(64 + (i % 32) * 16));
            }
            for h in holes {
                // SAFETY: allocated just above from this pool, freed once.
                unsafe { p.free(h) };
            }
            held
        }
    };
    let small = cpu_per_op(fragment(500), 112);
    let large = cpu_per_op(fragment(32_000), 112);
    assert!(
        large < small * MAX_RATIO,
        "allocation cost grew with fragmentation: {small:.1} at 500 free blocks, \
         {large:.1} at 32000 (thread CPU per op) — the pool is walking its free space"
    );
}
