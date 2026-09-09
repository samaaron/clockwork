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
//! degrading. A wall-clock ratio is a crude instrument, but the regression it
//! guards against was five orders of magnitude, so the bound can be loose
//! enough never to flake on a loaded machine and still catch it.

use std::ffi::c_void;
use std::time::Instant;
use clockwork_heap::HeapPool;

unsafe extern "C" fn new_area(size: usize) -> *mut c_void {
    let l = std::alloc::Layout::from_size_align(size, 16).unwrap();
    // SAFETY: the pool never asks for zero bytes.
    unsafe { std::alloc::alloc(l) }.cast::<c_void>()
}
unsafe extern "C" fn free_area(_p: *mut c_void) {}

fn pool() -> HeapPool {
    HeapPool::new(Some(new_area), Some(free_area), 48 * 1024 * 1024, 0)
}

/// Nanoseconds per alloc/free, best of three passes so a scheduling hiccup
/// does not decide the result.
fn ns_per_op(setup: impl Fn(&mut HeapPool) -> Vec<*mut u8>, size: usize) -> f64 {
    let mut best = f64::MAX;
    for _ in 0..3 {
        let mut p = pool();
        let held = setup(&mut p);
        let churn = 2000;
        let t = Instant::now();
        for _ in 0..churn {
            let q = p.alloc(size);
            assert!(!q.is_null());
            // SAFETY: just allocated from this pool, freed once.
            unsafe { p.free(q) };
        }
        let ns = t.elapsed().as_secs_f64() * 1e9 / (churn as f64 * 2.0);
        best = best.min(ns);
        for h in held {
            // SAFETY: each was allocated by `setup` from this pool.
            unsafe { p.free(h) };
        }
    }
    best
}

/// A generous bound: TLSF measures ~1x across these ranges and the linear
/// walk it replaced measured 90x and 170x.
const MAX_RATIO: f64 = 20.0;

#[test]
fn cost_is_flat_in_live_blocks() {
    let small = ns_per_op(|p| (0..500).map(|_| p.alloc(256)).collect(), 256);
    let large = ns_per_op(|p| (0..32_000).map(|_| p.alloc(256)).collect(), 256);
    assert!(
        large < small * MAX_RATIO,
        "allocation cost grew with occupancy: {small:.1} ns at 500 live blocks, \
         {large:.1} ns at 32000 — the pool is scanning what it holds"
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
    let small = ns_per_op(fragment(500), 112);
    let large = ns_per_op(fragment(32_000), 112);
    assert!(
        large < small * MAX_RATIO,
        "allocation cost grew with fragmentation: {small:.1} ns at 500 free blocks, \
         {large:.1} ns at 32000 — the pool is walking its free space"
    );
}
