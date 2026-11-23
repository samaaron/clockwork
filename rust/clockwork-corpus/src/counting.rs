// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! An allocator that counts, for a test that something allocates nothing.
//!
//! A test binary makes it its allocator:
//!
//! ```ignore
//! #[global_allocator]
//! static COUNTING: clockwork_corpus::counting::Counting = clockwork_corpus::counting::Counting;
//! ```
//!
//! and asks [`allocations_in`] how many allocations a piece of work made. The
//! count is the calling thread's, so the binary's other tests, on their own
//! threads, do not disturb it; work that allocates on another thread is not
//! counted. A test that reads a zero first shows the counter counting, with
//! work that does allocate.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

thread_local! {
    // Const-made and without a destructor: reading it allocates nothing and
    // is safe while a thread is being made or taken down.
    static ALLOCATIONS: Cell<usize> = const { Cell::new(0) };
}

fn count() {
    let _ = ALLOCATIONS.try_with(|n| n.set(n.get() + 1));
}

/// The system's allocator, counting each allocation and each reallocation
/// made on the thread that asks.
pub struct Counting;

// SAFETY: every call goes to the system allocator unchanged; this only counts.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count();
        // SAFETY: the caller's contract, passed on.
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: the caller's contract, passed on.
        unsafe { System.dealloc(ptr, layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        count();
        // SAFETY: the caller's contract, passed on.
        unsafe { System.realloc(ptr, layout, size) }
    }
}

/// Whether this binary's allocator is [`Counting`]: one allocation, counted.
/// An optimised build may take out an allocation that nothing looks at, and
/// then a working counter reads 0, so the vector is hidden from the optimiser.
pub fn counts() -> bool {
    allocations_in(|| drop(std::hint::black_box(Vec::<u8>::with_capacity(16)))) == 1
}

/// How many times `work` allocated on this thread. Always 0 in a binary
/// whose allocator is not [`Counting`], so a test proves the counter first.
pub fn allocations_in(work: impl FnOnce()) -> usize {
    let before = ALLOCATIONS.with(Cell::get);
    work();
    ALLOCATIONS.with(Cell::get) - before
}
