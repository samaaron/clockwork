// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The growable pool behind `clockwork_heap`.
//!
//! clockwork pre-allocates a block of memory at boot and suballocates from
//! it, so a running engine never calls the system allocator on a path that
//! matters. This is that suballocator: a two-level segregated fit over a chain
//! of areas, with coalescing, growing by asking for another area only when the
//! ones it has cannot satisfy a request.
//!
//! Nothing about the job is specific to a synthesis engine.
//!
//! # Layout
//!
//! Each area begins with an [`Area`] header, followed by a chain of blocks.
//! Every block carries its payload size, the size of the block physically
//! before it, whether it is free, and whether it ends the area:
//!
//! ```text
//!   [Area][hdr|payload…][hdr|payload…]…
//! ```
//!
//! # Why the cost is constant
//!
//! Because on the audio thread the worst case is the only case that matters.
//! An allocator whose cost grows with what it already holds does not get
//! slower gracefully: a few hundred live synths took a first-fit walk from
//! nanoseconds to hundreds of microseconds, that blew the block deadline, the
//! engine could not retire voices fast enough to recover, and it stopped
//! dead. Bounded time is a correctness property here, not a performance one.
//!
//! So free blocks are filed by size class rather than searched for. A request
//! is mapped to a class, rounded up so that anything in the class it lands in
//! is big enough, and the first non-empty class at or above it is found with
//! two bit scans over the level bitmaps — no free list is ever walked. Freeing
//! is constant too: the boundary tags in the block header name both physical
//! neighbours, so coalescing looks at exactly two blocks rather than walking
//! the area. `tests/constant_time.rs` guards the property; the invariants the
//! index and the tags must satisfy are checked exhaustively by the torture
//! test below.
//!
//! # Why this is unsafe code in a safe language
//!
//! An allocator hands out raw memory; there is no way to express that in safe
//! Rust. What Rust buys here is not the absence of `unsafe` but its
//! confinement: every pointer computation lives in this file, behind an API
//! that cannot be misused from outside, and the invariants are stated once
//! rather than re-derived at each call site.
//!
//! That one statement is this: **every block pointer in this file points at
//! a block header inside one of the pool's areas.** The chain is built by
//! `add_area`, split only by `alloc`, merged only by `free`, and a block's
//! `size`, `prev_size` and `last` tags describe its physical neighbours
//! exactly (the torture test checks all of it). Each `SAFETY` comment below
//! says which part of that it relies on.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

#[allow(unused_imports)]
use alloc::boxed::Box;
use core::ffi::c_void;

#[cfg(feature = "std")]
pub mod router;

/// Bytes a pool spends on its own bookkeeping at the head of every area.
///
/// A caller sizing a backing block adds this to the space it wants usable,
/// the way it did for the pool this replaces.
pub const AREA_OVERHEAD: usize = 64;

/// Every payload is 16-byte aligned: the ugens' SIMD paths assume at least
/// that of anything handed to them.
const ALIGN: usize = 16;

const fn align_up(n: usize) -> usize {
    (n + (ALIGN - 1)) & !(ALIGN - 1)
}

/// Asks the host for another area of at least `size` bytes. Returns null when
/// it cannot, which the pool reports as a failed allocation rather than
/// treating as fatal.
pub type NewAreaFunc = Option<unsafe extern "C" fn(size: usize) -> *mut c_void>;
/// Hands an area back. Called only for areas that are completely free.
pub type FreeAreaFunc = Option<unsafe extern "C" fn(area: *mut c_void)>;

/// The header at the start of every block. Sized so the payload that follows
/// it stays 16-byte aligned.
///
/// `prev_size` and `last` are what make coalescing O(1) in both directions:
/// the physically previous block is `BLOCK_HDR + prev_size` bytes back, and
/// `last` says whether stepping forward would leave the area. Both fit in the
/// padding the alignment already required, so the header is still 16 bytes on
/// 32- and 64-bit alike and no area loses capacity to them.
#[repr(C, align(16))]
struct Block {
    /// Payload bytes, always a multiple of [`ALIGN`].
    size: usize,
    /// Payload bytes of the block physically before this one; 0 when this is
    /// the first block in its area.
    prev_size: u32,
    free: bool,
    /// True when this is the last block in its area.
    last: bool,
}

/// The header at the start of every area.
#[repr(C, align(16))]
struct Area {
    next: *mut Area,
    /// Total bytes of this area, this header included.
    size: usize,
    /// False for the first area, whose memory the host supplied.
    owns_memory: bool,
}

const BLOCK_HDR: usize = core::mem::size_of::<Block>();
const AREA_HDR: usize = core::mem::size_of::<Area>();

// The header sizes must fit inside the overhead the public constant promises,
// or a caller that sized its backing block by that constant comes up short.
const _: () = assert!(
    AREA_HDR + BLOCK_HDR <= AREA_OVERHEAD,
    "AREA_OVERHEAD must cover an area header and its first block header"
);

/// A free block keeps its free-list links in its own payload, which by
/// definition nothing else is using. Two pointers must fit, and they do:
/// every payload is at least [`ALIGN`] bytes.
const _: () = assert!(
    ALIGN >= 2 * core::mem::size_of::<*mut u8>(),
    "a free block's payload must hold two free-list links"
);

// ── The size-class index: two-level segregated fit ──────────────────────────
//
// One free list per size class, and a bitmap per level saying which classes
// are non-empty. Allocation maps the request to a class, rounds up so the
// class it lands in is guaranteed to fit, and then finds the first non-empty
// class at or above it with two bit scans. No list is ever walked. The cost
// of an allocation is therefore the same whether the pool holds one free
// block or a hundred thousand — which is the property an audio thread needs,
// where the worst case is the only case that matters.
//
// Coalescing is the boundary tags above: a freed block merges with its
// physical neighbours in constant time, so bounded time does not come at the
// price of unbounded fragmentation.

/// Second-level classes per power of two. Sixteen keeps the index small and
/// bounds the waste of a good-fit round-up to about 1/16 of the request.
const SL_LOG2: usize = 4;
const SL_COUNT: usize = 1 << SL_LOG2;
/// `ALIGN` is 16, so every size is a multiple of 1 << 4.
const ALIGN_LOG2: usize = 4;
/// Sizes below this are all in first-level class 0, subdivided linearly:
/// below it the power-of-two spacing would be coarser than the alignment.
const FL_SHIFT: usize = SL_LOG2 + ALIGN_LOG2;
const SMALL_BLOCK: usize = 1 << FL_SHIFT;
/// Enough first-level classes to index any block a 32-bit size can express.
const FL_COUNT: usize = 25;

/// Index of the highest set bit.
#[inline]
fn fls(x: usize) -> usize {
    (usize::BITS - 1 - x.leading_zeros()) as usize
}

/// The class a block of `size` belongs in.
#[inline]
fn mapping_insert(size: usize) -> (usize, usize) {
    if size < SMALL_BLOCK {
        (0, size >> ALIGN_LOG2)
    } else {
        let f = fls(size);
        (f - FL_SHIFT + 1, (size >> (f - SL_LOG2)) - SL_COUNT)
    }
}

/// The class to allocate `size` from: rounded up so that ANY block in the
/// class it names is large enough, which is what makes the search itself
/// constant time — no list is inspected to check a fit.
#[inline]
fn mapping_search(size: usize) -> (usize, usize) {
    if size >= SMALL_BLOCK {
        let round = (1usize << (fls(size) - SL_LOG2)) - 1;
        mapping_insert(size + round)
    } else {
        mapping_insert(size)
    }
}

// Read/write the free-list links stored in a free block's payload.
//
// The caller passes a free block: nothing else is using its payload, and the
// `ALIGN` assertion above guarantees it holds two pointers.

/// Where a free block keeps its links: `next` first, `prev` one pointer on.
///
/// # Safety
/// `b` must be a block header inside one of the pool's areas.
unsafe fn links(b: *mut Block) -> *mut *mut Block {
    // SAFETY: the payload starts inside the block, per the contract.
    unsafe { Block::payload(b).cast::<*mut Block>() }
}
/// # Safety
/// `b` must be a free block.
unsafe fn fl_next(b: *mut Block) -> *mut Block {
    // SAFETY: a free block's payload holds its links, and the pointers are
    // aligned because every payload is.
    unsafe { links(b).read() }
}
/// # Safety
/// `b` must be a free block.
unsafe fn fl_set_next(b: *mut Block, v: *mut Block) {
    // SAFETY: as for `fl_next`.
    unsafe { links(b).write(v) }
}
/// # Safety
/// `b` must be a free block.
unsafe fn fl_prev(b: *mut Block) -> *mut Block {
    // SAFETY: as for `fl_next`; the second link is one pointer on, still
    // inside the payload.
    unsafe { links(b).add(1).read() }
}
/// # Safety
/// `b` must be a free block.
unsafe fn fl_set_prev(b: *mut Block, v: *mut Block) {
    // SAFETY: as for `fl_prev`.
    unsafe { links(b).add(1).write(v) }
}

impl Block {
    /// The payload this block hands out.
    ///
    /// # Safety
    /// `b` must be a block header inside one of the pool's areas.
    unsafe fn payload(b: *mut Block) -> *mut u8 {
        // SAFETY: a block is its header followed by at least `ALIGN` bytes of
        // payload, so the header's end is inside the same area.
        unsafe { b.cast::<u8>().add(BLOCK_HDR) }
    }

    /// The block that starts where this one's payload ends.
    ///
    /// # Safety
    /// `b` must be a block header inside one of the pool's areas.
    unsafe fn next(b: *mut Block) -> *mut Block {
        // SAFETY: the payload ends inside the area (`size` is the tag the
        // chain was built with), and when `b` is the last block the result
        // is one past the area's end, which is still a valid pointer to form.
        unsafe { Block::payload(b).add((*b).size).cast::<Block>() }
    }
}

/// A segregated-fit pool over a chain of host-supplied areas.
pub struct HeapPool {
    new_area: NewAreaFunc,
    free_area: FreeAreaFunc,
    growth_size: usize,
    total_area: usize,
    areas: *mut Area,
    /// Bit `i` set when first-level class `i` has any non-empty second-level
    /// class.
    fl_bitmap: u32,
    /// Bit `j` of `sl_bitmap[i]` set when class `(i, j)` has a free block.
    sl_bitmap: [u32; FL_COUNT],
    /// Head of the free list for each class.
    heads: [[*mut Block; SL_COUNT]; FL_COUNT],
}

impl HeapPool {
    /// `initial_size` is requested immediately; `growth_size` is the minimum
    /// size of any later area. A request larger than `growth_size` asks for
    /// exactly what it needs, so a single large allocation cannot be refused
    /// merely for exceeding the growth quantum.
    pub fn new(
        new_area: NewAreaFunc,
        free_area: FreeAreaFunc,
        initial_size: usize,
        growth_size: usize,
    ) -> HeapPool {
        let mut pool = HeapPool {
            new_area,
            free_area,
            growth_size,
            total_area: 0,
            areas: core::ptr::null_mut(),
            fl_bitmap: 0,
            sl_bitmap: [0; FL_COUNT],
            heads: [[core::ptr::null_mut(); SL_COUNT]; FL_COUNT],
        };
        pool.add_area(initial_size);
        pool
    }

    /// Bytes handed to the pool across all its areas.
    pub fn total_area(&self) -> usize {
        self.total_area
    }

    /// Put a block on its size class's free list. Constant time: the class
    /// is computed, not searched for.
    ///
    /// # Safety
    /// `b` must be a block in one of this pool's areas, already marked free
    /// and on no list.
    unsafe fn fl_push(&mut self, b: *mut Block) {
        // SAFETY: `b` is free per the contract, and `head` is free because
        // only free blocks are ever on a list.
        unsafe {
            let (f, l) = mapping_insert((*b).size);
            let head = self.heads[f][l];
            fl_set_prev(b, core::ptr::null_mut());
            fl_set_next(b, head);
            if !head.is_null() {
                fl_set_prev(head, b);
            }
            self.heads[f][l] = b;
            self.sl_bitmap[f] |= 1u32 << l;
            self.fl_bitmap |= 1u32 << f;
        }
    }

    /// Take a block off its free list, clearing the class's bits when it was
    /// the last one there. Constant time.
    ///
    /// # Safety
    /// `b` must be a free block on this pool's list for its class.
    unsafe fn fl_remove(&mut self, b: *mut Block) {
        // SAFETY: `b` and both its neighbours are on a list, so all are free.
        unsafe {
            let (f, l) = mapping_insert((*b).size);
            let (p, n) = (fl_prev(b), fl_next(b));
            if p.is_null() {
                self.heads[f][l] = n;
                if n.is_null() {
                    self.sl_bitmap[f] &= !(1u32 << l);
                    if self.sl_bitmap[f] == 0 {
                        self.fl_bitmap &= !(1u32 << f);
                    }
                }
            } else {
                fl_set_next(p, n);
            }
            if !n.is_null() {
                fl_set_prev(n, p);
            }
        }
    }

    /// The first non-empty class at or above `(f, l)`, found with two bit
    /// scans and no list traversal. `None` when the pool has nothing that
    /// large.
    fn find_suitable(&self, f: usize, l: usize) -> Option<(usize, usize)> {
        // Classes above `l` within this first level...
        let mut sl_map = self.sl_bitmap[f] & (!0u32 << l);
        let mut f2 = f;
        if sl_map == 0 {
            // ...else the next non-empty first level, whose every class is
            // larger than anything in `f`.
            let fl_map = self.fl_bitmap & (!0u32 << (f + 1));
            if fl_map == 0 {
                return None;
            }
            f2 = fl_map.trailing_zeros() as usize;
            sl_map = self.sl_bitmap[f2];
        }
        Some((f2, sl_map.trailing_zeros() as usize))
    }

    fn add_area(&mut self, size: usize) -> *mut Area {
        let Some(new_area) = self.new_area else { return core::ptr::null_mut() };
        // Room for the area header and one block header, or the area is
        // useless.
        if size <= AREA_HDR + BLOCK_HDR {
            return core::ptr::null_mut();
        }

        // Asked for exactly this many bytes and given exactly this many:
        // rounding the request up here would have the pool believe in space
        // the caller never allocated, and it would write past the end of the
        // block. That is not hypothetical — it corrupted the system heap the
        // first time.
        // SAFETY: the host's callback has no preconditions beyond a size.
        let mem = unsafe { new_area(size) };
        if mem.is_null() {
            return core::ptr::null_mut();
        }

        let area = mem.cast::<Area>();
        // SAFETY: the host gave `size` bytes, checked above to hold both
        // headers, and `NewAreaFunc` promises them 16-aligned.
        unsafe {
            (*area).next = self.areas;
            (*area).size = size;
            (*area).owns_memory = !self.areas.is_null();
        }
        self.areas = area;
        self.total_area += size;

        // The usable span is whatever is left after the two headers, rounded
        // down so every payload stays aligned and no block claims a partial
        // word.
        // SAFETY: the area is set up above; its one block is free and on no
        // list yet.
        unsafe {
            let b = first_block(area);
            (*b).size = (size - AREA_HDR - BLOCK_HDR) & !(ALIGN - 1);
            (*b).free = true;
            (*b).prev_size = 0;
            (*b).last = true;
            self.fl_push(b);
        }
        area
    }

    /// Memory for `bytes`, or null when no area can be found or grown to hold
    /// it. The returned pointer is 16-byte aligned.
    pub fn alloc(&mut self, bytes: usize) -> *mut u8 {
        if bytes == 0 {
            return core::ptr::null_mut();
        }
        let want = align_up(bytes);

        for attempt in 0..2 {
            // SAFETY: `b` comes off a free list, so it is a free block in one
            // of the areas; `rest` is carved from inside `b`'s payload, which
            // the size check leaves room for; `Block::next(rest)` exists
            // because `rest` is not last.
            unsafe {
                let (f, l) = mapping_search(want);
                if let Some((f2, l2)) = self.find_suitable(f, l) {
                    let b = self.heads[f2][l2];
                    // Guaranteed by the round-up in mapping_search: every
                    // block in the class found is at least `want` bytes.
                    debug_assert!((*b).size >= want);
                    self.fl_remove(b);

                    // Split only when the remainder can hold a block of
                    // its own; otherwise the caller gets the few spare
                    // bytes, which is cheaper than tracking a fragment
                    // nothing can use.
                    let spare = (*b).size - want;
                    if spare > BLOCK_HDR + ALIGN {
                        let rest = Block::payload(b).add(want).cast::<Block>();
                        (*rest).size = spare - BLOCK_HDR;
                        (*rest).free = true;
                        (*rest).prev_size = want as u32;
                        (*rest).last = (*b).last;
                        (*b).size = want;
                        (*b).last = false;
                        if !(*rest).last {
                            (*Block::next(rest)).prev_size = (*rest).size as u32;
                        }
                        self.fl_push(rest);
                    }
                    (*b).free = false;
                    return Block::payload(b);
                }
            }

            // Nothing fitted. Ask for another area — at least the growth
            // quantum, but big enough for this request whatever its size.
            if attempt == 0 {
                let need = want + AREA_HDR + BLOCK_HDR;
                let size = if need > self.growth_size { need } else { self.growth_size };
                if self.add_area(size).is_null() {
                    return core::ptr::null_mut();
                }
            }
        }
        core::ptr::null_mut()
    }

    /// Give memory back.
    ///
    /// # Safety
    /// `ptr` must be null or a pointer this pool returned from [`alloc`] and
    /// has not already been given back.
    ///
    /// [`alloc`]: HeapPool::alloc
    pub unsafe fn free(&mut self, ptr: *mut u8) {
        if ptr.is_null() {
            return;
        }
        // SAFETY: `ptr` is a payload this pool handed out (the contract), so
        // its header is `BLOCK_HDR` bytes back. The neighbours are reached
        // through the boundary tags, which name them exactly, and `last`
        // says when there is no successor.
        unsafe {
            let mut b = ptr.sub(BLOCK_HDR).cast::<Block>();
            (*b).free = true;

            // Coalesce with the physical neighbours, both in constant time:
            // the successor is one step forward, the predecessor is what
            // prev_size names. The previous version walked the whole area on
            // every call, excused by "free is not on the audio path" — it
            // is. Every synth that ends frees from the block callback, and
            // that walk is what made a few hundred voices cost milliseconds
            // apiece.
            if !(*b).last {
                let n = Block::next(b);
                if (*n).free {
                    self.fl_remove(n);
                    (*b).size += BLOCK_HDR + (*n).size;
                    (*b).last = (*n).last;
                }
            }
            if (*b).prev_size != 0 {
                let p = b.cast::<u8>().sub(BLOCK_HDR + (*b).prev_size as usize).cast::<Block>();
                if (*p).free {
                    self.fl_remove(p);
                    (*p).size += BLOCK_HDR + (*b).size;
                    (*p).last = (*b).last;
                    b = p;
                }
            }
            if !(*b).last {
                (*Block::next(b)).prev_size = (*b).size as u32;
            }
            self.fl_push(b);
        }
    }

    /// Return every area to a single free block, abandoning all outstanding
    /// allocations. Used when the DSP is being rebuilt and nothing from the
    /// previous one survives.
    pub fn free_all_internal(&mut self) {
        self.fl_bitmap = 0;
        self.sl_bitmap = [0; FL_COUNT];
        self.heads = [[core::ptr::null_mut(); SL_COUNT]; FL_COUNT];
        let mut a = self.areas;
        while !a.is_null() {
            // SAFETY: every area on the chain was set up by `add_area`, and
            // the index was just emptied, so the block is on no list.
            unsafe {
                let b = first_block(a);
                (*b).size = ((*a).size - AREA_HDR - BLOCK_HDR) & !(ALIGN - 1);
                (*b).free = true;
                (*b).prev_size = 0;
                (*b).last = true;
                self.fl_push(b);
                a = (*a).next;
            }
        }
    }
}

impl Drop for HeapPool {
    fn drop(&mut self) {
        let mut a = self.areas;
        while !a.is_null() {
            // SAFETY: an area on the chain; the link is read before the host
            // gets the memory back.
            let next = unsafe { (*a).next };
            if let Some(free_area) = self.free_area {
                // SAFETY: the area came from `new_area` and is handed back
                // exactly once, here.
                unsafe { free_area(a.cast::<c_void>()) };
            }
            a = next;
        }
        self.areas = core::ptr::null_mut();
    }
}

/// # Safety
/// `area` must be an area header set up by `add_area`.
unsafe fn first_block(area: *mut Area) -> *mut Block {
    // SAFETY: `add_area` refuses any area too small for both headers.
    unsafe { area.cast::<u8>().add(AREA_HDR).cast::<Block>() }
}

// ── C ABI ───────────────────────────────────────────────────────────────────
//
// The pool is reached from C++ (`clockwork_heap.cpp`), which owns the
// lifetime and the lock. An opaque handle rather than a struct, so the layout
// stays this crate's business.

/// Create a pool. Returns null if the initial area could not be obtained.
///
/// # Safety
/// The callbacks must behave as [`NewAreaFunc`] and [`FreeAreaFunc`] say.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_pool_new(
    new_area: NewAreaFunc,
    free_area: FreeAreaFunc,
    initial_size: usize,
    growth_size: usize,
) -> *mut c_void {
    let pool = Box::new(HeapPool::new(new_area, free_area, initial_size, growth_size));
    Box::into_raw(pool).cast::<c_void>()
}

/// Destroy a pool, handing every area back through its free callback.
///
/// # Safety
/// `pool` must be null or a handle from [`clockwork_heap_pool_new`] that has
/// not been destroyed, and nothing may use it afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_pool_free(pool: *mut c_void) {
    if !pool.is_null() {
        // SAFETY: the handle is the box `clockwork_heap_pool_new` leaked, and
        // this is its one owner from here on.
        unsafe { drop(Box::from_raw(pool.cast::<HeapPool>())) };
    }
}

/// # Safety
/// `pool` must be null or a live handle, and the caller must hold whatever
/// lock keeps it to one thread at a time.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_pool_alloc(pool: *mut c_void, bytes: usize) -> *mut c_void {
    if pool.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: a live handle, used by one thread at a time, per the contract.
    unsafe { (*pool.cast::<HeapPool>()).alloc(bytes).cast::<c_void>() }
}

/// # Safety
/// As for [`clockwork_heap_pool_alloc`], and `ptr` must be null or a payload
/// this pool returned that has not already been given back.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_pool_dealloc(pool: *mut c_void, ptr: *mut c_void) {
    if pool.is_null() {
        return;
    }
    // SAFETY: the contract above is exactly `HeapPool::free`'s.
    unsafe { (*pool.cast::<HeapPool>()).free(ptr.cast::<u8>()) };
}

/// # Safety
/// As for [`clockwork_heap_pool_alloc`]; every payload the pool handed out
/// is dead afterwards.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_pool_free_all(pool: *mut c_void) {
    if !pool.is_null() {
        // SAFETY: a live handle, used by one thread at a time, per the contract.
        unsafe { (*pool.cast::<HeapPool>()).free_all_internal() };
    }
}

/// # Safety
/// `pool` must be null or a live handle.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_pool_total_area(pool: *const c_void) -> usize {
    if pool.is_null() {
        return 0;
    }
    // SAFETY: a live handle, per the contract.
    unsafe { (*pool.cast::<HeapPool>()).total_area() }
}

/// What a caller must add to the space it wants usable when sizing a backing
/// block.
#[no_mangle]
pub extern "C" fn clockwork_heap_pool_area_overhead() -> usize {
    AREA_OVERHEAD
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;
    use std::sync::Mutex;

    // A host that hands out plain heap memory and remembers what it gave, so
    // a test can assert the pool asked for and returned the right areas.
    static AREAS: Mutex<Option<HashMap<usize, usize>>> = Mutex::new(None);

    unsafe extern "C" fn test_new_area(size: usize) -> *mut c_void {
        let layout = std::alloc::Layout::from_size_align(size, ALIGN).unwrap();
        // SAFETY: the pool never asks for zero bytes.
        let p = unsafe { std::alloc::alloc(layout) };
        AREAS.lock().unwrap().get_or_insert_with(HashMap::new).insert(p as usize, size);
        p.cast::<c_void>()
    }

    unsafe extern "C" fn test_free_area(area: *mut c_void) {
        let mut g = AREAS.lock().unwrap();
        let map = g.get_or_insert_with(HashMap::new);
        if let Some(size) = map.remove(&(area as usize)) {
            let layout = std::alloc::Layout::from_size_align(size, ALIGN).unwrap();
            // SAFETY: `test_new_area` allocated exactly this with this layout,
            // and the map entry is gone so it cannot be freed twice.
            unsafe { std::alloc::dealloc(area.cast::<u8>(), layout) };
        }
    }

    fn pool(initial: usize, growth: usize) -> HeapPool {
        HeapPool::new(Some(test_new_area), Some(test_free_area), initial, growth)
    }

    // ── The structural oracle ──────────────────────────────────────────────
    //
    // An allocator's real specification is its own invariants, and they are
    // checkable in full: the block chain tiles each area exactly, the
    // boundary tags agree with the chain, coalescing has left no two free
    // blocks adjacent, and the size-class index describes precisely the set
    // of free blocks that exist. Anything that corrupts the heap breaks one
    // of these, usually long before it produces a wrong pointer — which is
    // what makes this a better oracle than comparing against another
    // allocator, whose placement policy is allowed to differ from ours.

    /// Walk every area and every free list and assert the pool is exactly
    /// what it claims to be. `live` maps a payload pointer to the size that
    /// was asked for, so allocated blocks can be checked against it too.
    fn assert_invariants(p: &HeapPool, live: &std::collections::HashMap<usize, usize>) {
        let mut free_seen: std::collections::HashSet<usize> = Default::default();

        // 1. Each area's blocks tile it, tags agree, no adjacent free pair.
        let mut a = p.areas;
        while !a.is_null() {
            // SAFETY: this walks the chain by the same tags the pool uses,
            // and asserts each step stays inside the area before taking it.
            unsafe {
                let end = a.cast::<u8>().add((*a).size);
                let mut b = first_block(a);
                let mut prev: *mut Block = core::ptr::null_mut();
                loop {
                    assert!(b.cast::<u8>() < end, "block chain ran past its area");
                    assert_eq!((*b).size % ALIGN, 0, "block size not aligned");
                    assert!((*b).size >= ALIGN, "block smaller than the alignment");
                    assert_eq!(
                        Block::payload(b) as usize % ALIGN,
                        0,
                        "payload not aligned"
                    );

                    // Boundary tag matches the chain we walked.
                    if prev.is_null() {
                        assert_eq!((*b).prev_size, 0, "first block claims a predecessor");
                    } else {
                        assert_eq!(
                            (*b).prev_size as usize,
                            (*prev).size,
                            "prev_size disagrees with the previous block"
                        );
                    }

                    if (*b).free {
                        // Coalescing is complete.
                        if !prev.is_null() {
                            assert!(!(*prev).free, "two adjacent free blocks");
                        }
                        free_seen.insert(b as usize);
                    } else if let Some(asked) = live.get(&(Block::payload(b) as usize)) {
                        assert!(
                            (*b).size >= align_up(*asked),
                            "live block smaller than the request it satisfied"
                        );
                    }

                    if (*b).last {
                        assert!(
                            Block::next(b).cast::<u8>() <= end,
                            "last block overruns its area"
                        );
                        break;
                    }
                    prev = b;
                    b = Block::next(b);
                }
                a = (*a).next;
            }
        }

        // 2. The index describes exactly the free blocks that exist, each in
        //    the class its size maps to, and the bitmaps match the lists.
        let mut indexed: std::collections::HashSet<usize> = Default::default();
        for f in 0..FL_COUNT {
            let sl_nonempty = p.sl_bitmap[f] != 0;
            assert_eq!(
                sl_nonempty,
                p.fl_bitmap & (1u32 << f) != 0,
                "fl_bitmap disagrees with sl_bitmap[{f}]"
            );
            for l in 0..SL_COUNT {
                let head = p.heads[f][l];
                assert_eq!(
                    !head.is_null(),
                    p.sl_bitmap[f] & (1u32 << l) != 0,
                    "sl_bitmap[{f}] bit {l} disagrees with its list"
                );
                let mut b = head;
                let mut back: *mut Block = core::ptr::null_mut();
                while !b.is_null() {
                    // SAFETY: only blocks in the areas are ever put on a list.
                    unsafe {
                        assert!((*b).free, "an allocated block is on a free list");
                        assert_eq!(
                            mapping_insert((*b).size),
                            (f, l),
                            "free block filed under the wrong size class"
                        );
                        assert_eq!(fl_prev(b), back, "free-list back link is wrong");
                        assert!(
                            indexed.insert(b as usize),
                            "a free block appears on the index twice"
                        );
                        back = b;
                        b = fl_next(b);
                    }
                }
            }
        }
        assert_eq!(
            indexed, free_seen,
            "the index and the heap disagree about which blocks are free"
        );
    }

    /// Deterministic xorshift — a seeded sequence, so a failure reproduces.
    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }
        fn below(&mut self, n: usize) -> usize {
            (self.next() % n as u64) as usize
        }
    }

    /// Random alloc/free/free_all against the invariants, with every live
    /// payload carrying a pattern that is verified before it is handed back —
    /// so an overlap or a stale reuse is caught as corrupted bytes even when
    /// the structure still looks well formed.
    fn torture(seed: u64, ops: usize, check_every: usize) {
        let mut p = pool(1024 * 1024, 64 * 1024);
        let mut rng = Rng(seed);
        // payload ptr -> (asked size, pattern byte)
        let mut live: std::collections::HashMap<usize, (usize, u8)> = Default::default();
        let mut asked_only: std::collections::HashMap<usize, usize> = Default::default();

        for i in 0..ops {
            let roll = rng.below(100);
            if roll < 55 || live.is_empty() {
                // Mostly small, occasionally large enough to force growth.
                let n = match rng.below(20) {
                    0 => 1 + rng.below(200_000),
                    1..=4 => 1 + rng.below(4096),
                    _ => 1 + rng.below(512),
                };
                let q = p.alloc(n);
                if q.is_null() {
                    continue; // refusal is a legal outcome, not a fault
                }
                assert_eq!(q as usize % ALIGN, 0);
                let pat = (i % 251) as u8 + 1;
                // SAFETY: `q` is a fresh payload of at least `n` bytes.
                unsafe { core::ptr::write_bytes(q, pat, n) };
                assert!(
                    live.insert(q as usize, (n, pat)).is_none(),
                    "the pool handed out a pointer it had already handed out"
                );
                asked_only.insert(q as usize, n);
            } else if roll < 99 {
                let victim = *live.keys().nth(rng.below(live.len())).unwrap();
                let (n, pat) = live.remove(&victim).unwrap();
                asked_only.remove(&victim);
                // SAFETY: `victim` is live: allocated with `n` bytes, filled,
                // and not yet freed.
                let bytes = unsafe { core::slice::from_raw_parts(victim as *const u8, n) };
                assert!(
                    bytes.iter().all(|&b| b == pat),
                    "a live allocation was corrupted while it was held"
                );
                // SAFETY: freed exactly once; it just left `live`.
                unsafe { p.free(victim as *mut u8) };
            } else {
                p.free_all_internal();
                live.clear();
                asked_only.clear();
            }

            if i % check_every == 0 {
                assert_invariants(&p, &asked_only);
            }
        }
        assert_invariants(&p, &asked_only);
    }

    #[test]
    fn torture_holds_the_invariants() {
        // Miri runs this too, where it is the slowest thing in the suite —
        // so it gets a shorter run there and the native run carries the
        // volume.
        let (ops, seeds) = if cfg!(miri) { (150, 1) } else { (60_000, 8) };
        for s in 0..seeds {
            torture(0x9E3779B97F4A7C15 ^ (s as u64 + 1), ops, 64);
        }
    }

    #[test]
    fn allocations_are_aligned_and_distinct() {
        let mut p = pool(64 * 1024, 64 * 1024);
        let mut seen = Vec::new();
        for n in [1usize, 15, 16, 17, 100, 1000] {
            let q = p.alloc(n);
            assert!(!q.is_null(), "alloc({n})");
            assert_eq!(q as usize % ALIGN, 0, "alloc({n}) alignment");
            assert!(!seen.contains(&(q as usize)), "alloc({n}) handed out twice");
            seen.push(q as usize);
        }
    }

    #[test]
    fn zero_bytes_is_not_an_allocation() {
        let mut p = pool(64 * 1024, 64 * 1024);
        assert!(p.alloc(0).is_null());
    }

    #[test]
    fn payloads_do_not_overlap() {
        // Fill each allocation with a distinct byte, then check none of them
        // was disturbed: an overlap shows up as a byte from the wrong run.
        let mut p = pool(64 * 1024, 64 * 1024);
        let sizes = [32usize, 64, 128, 48, 512, 16];
        let mut blocks = Vec::new();
        for (i, &n) in sizes.iter().enumerate() {
            let q = p.alloc(n);
            assert!(!q.is_null());
            // SAFETY: a fresh payload of at least `n` bytes.
            unsafe { core::ptr::write_bytes(q, i as u8 + 1, n) };
            blocks.push((q, n, i as u8 + 1));
        }
        for (q, n, tag) in blocks {
            // SAFETY: still live, `n` bytes long, written above.
            let bytes = unsafe { core::slice::from_raw_parts(q, n) };
            assert!(bytes.iter().all(|&b| b == tag), "block tagged {tag} was disturbed");
        }
    }

    #[test]
    fn freed_memory_is_reused() {
        let mut p = pool(64 * 1024, 64 * 1024);
        let a = p.alloc(1024);
        // SAFETY: `a` came from this pool and is freed once.
        unsafe { p.free(a) };
        let b = p.alloc(1024);
        assert_eq!(a, b, "the same block should come back");
    }

    #[test]
    fn adjacent_frees_coalesce() {
        // Two neighbours freed become one block big enough for both. Asserting
        // the area count did not change is the point: without coalescing the
        // pool would satisfy this by growing, and the test would pass while
        // proving nothing.
        let mut p = pool(4096 + AREA_OVERHEAD, 64 * 1024);
        let a = p.alloc(1024);
        let b = p.alloc(1024);
        assert!(!a.is_null() && !b.is_null());
        let area = p.total_area();
        // SAFETY: both came from this pool and are freed once.
        unsafe {
            p.free(a);
            p.free(b);
        }
        let big = p.alloc(2048);
        assert!(!big.is_null(), "coalesced space should fit 2048");
        assert_eq!(p.total_area(), area, "it should have reused, not grown");
    }

    #[test]
    fn grows_when_the_first_area_is_full() {
        let mut p = pool(1024 + AREA_OVERHEAD, 64 * 1024);
        let before = p.total_area();
        // Far more than the initial area holds.
        let mut kept = Vec::new();
        for _ in 0..64 {
            let q = p.alloc(512);
            assert!(!q.is_null(), "should have grown rather than failed");
            kept.push(q);
        }
        assert!(p.total_area() > before, "growth should have added area");
    }

    #[test]
    fn a_request_larger_than_the_growth_quantum_still_fits() {
        // The growth area is sized to the request when the request is bigger,
        // so one large allocation is not refused for exceeding the quantum.
        let mut p = pool(1024 + AREA_OVERHEAD, 4096);
        let q = p.alloc(256 * 1024);
        assert!(!q.is_null());
    }

    #[test]
    fn failure_is_reported_rather_than_fatal() {
        unsafe extern "C" fn no_area(_size: usize) -> *mut c_void {
            core::ptr::null_mut()
        }
        let mut p = HeapPool::new(Some(no_area), None, 64 * 1024, 64 * 1024);
        assert!(p.alloc(16).is_null());
    }

    #[test]
    fn free_all_internal_reclaims_everything() {
        // Fill the initial area without letting it grow, abandon everything,
        // then take one block spanning most of it. Growth would also satisfy
        // that, so the area total has to be unchanged for it to mean anything.
        let mut p = pool(8192 + AREA_OVERHEAD, 64 * 1024);
        for _ in 0..24 {
            assert!(!p.alloc(256).is_null());
        }
        let area = p.total_area();
        p.free_all_internal();
        assert!(!p.alloc(8000).is_null(), "everything should be free again");
        assert_eq!(p.total_area(), area, "it should have reused, not grown");
    }

    #[test]
    fn survives_a_long_mixed_run() {
        // The shape that found the original bug: many allocations and frees
        // in a pattern that forces splitting, coalescing and growth. Every
        // payload is written and read back, so an overlap or an overrun shows
        // up as wrong data rather than as luck.
        let mut p = pool(32 * 1024 + AREA_OVERHEAD, 32 * 1024);
        let mut live: Vec<(*mut u8, usize, u8)> = Vec::new();
        let mut rng: u32 = 0x1234_5678;
        let mut next = || {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            rng
        };

        // Miri interprets every payload byte; the native run keeps the full
        // 20k-step churn that found the original bug.
        let steps: u32 = if cfg!(miri) { 600 } else { 20_000 };
        for step in 0..steps {
            let free_it = !live.is_empty() && (next() % 3 == 0);
            if free_it {
                let i = (next() as usize) % live.len();
                let (q, n, tag) = live.swap_remove(i);
                // SAFETY: live: allocated with `n` bytes, filled, not yet freed.
                let bytes = unsafe { core::slice::from_raw_parts(q, n) };
                assert!(bytes.iter().all(|&b| b == tag), "step {step}: block was disturbed");
                // SAFETY: freed once; it just left `live`.
                unsafe { p.free(q) };
            } else {
                let n = 1 + (next() as usize % 2048);
                let q = p.alloc(n);
                assert!(!q.is_null(), "step {step}: alloc({n}) failed");
                let tag = (step % 251) as u8 + 1;
                // SAFETY: a fresh payload of at least `n` bytes.
                unsafe { core::ptr::write_bytes(q, tag, n) };
                live.push((q, n, tag));
            }
        }

        for (q, n, tag) in live {
            // SAFETY: still live, `n` bytes long, written above.
            let bytes = unsafe { core::slice::from_raw_parts(q, n) };
            assert!(bytes.iter().all(|&b| b == tag), "final check: block was disturbed");
            // SAFETY: freed once, here.
            unsafe { p.free(q) };
        }
    }
}
