// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The RT router — the global allocator, which keeps each core's
//! allocations inside its core's memory.
//!
//! The rule this enforces: **no locks of any kind on the audio thread, and
//! no malloc except our own pools.** The render path allocates nothing;
//! this covers the control path — a guest's messages all execute on the
//! audio thread in every deployment of this engine, worklet and native
//! alike, and the `HashMap`s and `Vec`s a door makes for them would
//! otherwise reach the platform's malloc.
//!
//! Mechanism: each core has a heap of two pools ([`crate::core_heap`]): the
//! guest's, over the guest's bulk memory, and the core's own, over a block
//! the core takes at its first init. The thread's entry ([`crate::entry`])
//! names the core a call works inside, the kind of call, and whether it
//! holds the core's gate. An allocation by the gate's holder inside a guest
//! call comes from the guest's pool; inside a door (the FFI layer opens one
//! across every sink and port call that makes or frees something), from the
//! core's own. Every other allocation is the system's. A free finds its pool
//! by the address alone ([`crate::map`]), from any thread; a pool parks a
//! free made without the gate, and its holder takes it back at its next
//! allocation or free. Constant time throughout, whatever the pools hold,
//! and no atomic is contended: a pool has one thread at a time.
//!
//! Edges, stated rather than hidden:
//!
//! * **Exhaustion** falls back to the system allocator and *counts* it, per
//!   pool ([`crate::core_heap::PoolCensus`]), because a dropout beats an
//!   abort mid-performance. The census asserts the counts stay zero under
//!   test load; a nonzero count in production is a sizing bug made visible,
//!   not a silent lie.
//! * **Over-aligned requests** (align > 16) are served from a pool with a
//!   back-pointer stashed below the aligned address.
//! * Outside every core, without the gate, or before a core's pools run,
//!   everything passes through to the system allocator untouched — `cargo
//!   test` binaries and every thread a core does not own see the allocator
//!   they always had. A door on a thread without the gate (MIDI clock out's,
//!   the NRT gateway's) makes its queues there.
//!
//! A guest written in Rust allocates the Rust way and never learns any of
//! this: its `Box`es and `Vec`s land in its own memory because clockwork
//! calls it inside guest calls.

use std::alloc::{GlobalAlloc, Layout, System};

use crate::core_heap::{self, CoreHeap, Pool};
use crate::entry::{self, Entry, Kind};
use crate::map::MAP;

const POOL_ALIGN: usize = 16;

/// RAII: a door, open for the length of the scope. The FFI layer opens one
/// across every sink and port call that makes or frees what a core's own
/// pool serves; the thread keeps its core and its gate, and gets its entry
/// back on drop.
pub struct Door {
    previous: Entry,
}

impl Door {
    #[inline]
    pub fn enter() -> Door {
        Door { previous: entry::within(Kind::Door) }
    }
}

impl Drop for Door {
    #[inline]
    fn drop(&mut self) {
        entry::leave(self.previous);
    }
}

/// The pool an allocation of this kind comes from, by the gate's holder.
fn pool_for(kind: Kind) -> Option<Pool> {
    match kind {
        Kind::GuestBoot | Kind::GuestProcessing => Some(Pool::Guest),
        Kind::Door => Some(Pool::Own),
        Kind::Control | Kind::Render => None,
    }
}

/// `layout` from `pool`, or null. Over-aligned: slack taken, the address
/// aligned up, and the pool's own pointer stashed in the word below it.
fn take(heap: &CoreHeap, pool: Pool, layout: Layout) -> *mut u8 {
    if layout.align() <= POOL_ALIGN {
        return heap.take(pool, layout.size());
    }
    let raw = heap.take(pool, layout.size() + layout.align() + POOL_ALIGN);
    if raw.is_null() {
        return raw;
    }
    let aligned = raw.map_addr(|a| (a + POOL_ALIGN + layout.align() - 1) & !(layout.align() - 1));
    // SAFETY: the slack taken puts the aligned address and the word below it
    // inside the payload.
    unsafe { *aligned.cast::<*mut u8>().sub(1) = raw };
    aligned
}

/// The global allocator: a core's pools inside its calls, the system
/// everywhere else.
pub struct RtRouter;

// SAFETY: every pointer handed out is either the system allocator's or a
// pool payload of at least `layout.size()` bytes at `layout.align()`, and
// `dealloc` routes each back to the allocator it came from: by the map and
// the heap's exact spans, a block a pool holds goes back to that pool, and
// anything else to the system. A pool is only ever touched by the thread
// holding its core's gate, one thread at a time by the gate's own rule, and
// frees without it are parked rather than reaching it.
unsafe impl GlobalAlloc for RtRouter {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let here = entry::current();
        if here.gate {
            if let Some(pool) = pool_for(here.kind) {
                // SAFETY: an entry names a live core's heap, or none.
                if let Some(heap) = unsafe { here.heap.cast::<CoreHeap>().as_ref() } {
                    if heap.running(pool) {
                        let p = take(heap, pool, layout);
                        if !p.is_null() {
                            return p;
                        }
                        heap.fell_back(pool);
                    }
                }
            }
        }
        // SAFETY: the caller's contract is `System`'s.
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        let owner = MAP.owner_of(p as usize);
        // A block of a core that went with it still held: its span is kept,
        // and nobody's to free into (core_heap::tombstone).
        if owner == core_heap::tombstone().as_ptr() {
            core_heap::note_outlived_free();
            return;
        }
        // SAFETY: the map names a live core's heap: a core is freed only once
        // nothing is inside it, and what outlived one is the tombstone's.
        if let Some(heap) = unsafe { owner.cast::<CoreHeap>().as_ref() } {
            if let Some(pool) = heap.pool_of(p) {
                let block = if layout.align() <= POOL_ALIGN {
                    p
                } else {
                    // SAFETY: a pool's block: `alloc` stashed the pool's own
                    // pointer below every over-aligned address it handed out.
                    unsafe { *p.cast::<*mut u8>().sub(1) }
                };
                // SAFETY: a payload the pool handed out, dead to its owner.
                unsafe { heap.give(pool, block) };
                return;
            }
        }
        // SAFETY: no pool holds it, so `alloc` got it from `System` with this
        // layout.
        unsafe { System.dealloc(p, layout) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::map::CHUNK_BYTES;
    use std::ptr;

    // A core's heap with both pools running over spans of their own, as a
    // core's first init leaves it, stopped as the core goes. Held by its
    // pointer, as a core holds it: the map names it, so it never moves.
    struct Core {
        heap: ptr::NonNull<CoreHeap>,
        spans: [(*mut u8, Layout); 2],
    }
    impl Core {
        fn new(bytes: usize) -> Core {
            let layout = Layout::from_size_align(bytes, CHUNK_BYTES).unwrap();
            // SAFETY: non-zero sizes.
            let spans = unsafe { [(System.alloc(layout), layout), (System.alloc(layout), layout)] };
            assert!(!spans[0].0.is_null() && !spans[1].0.is_null());
            let core = Core { heap: ptr::NonNull::from(Box::leak(Box::new(CoreHeap::new()))), spans };
            let held = core.call(Kind::Control, true);
            // SAFETY: the spans are the heap's until `drop`.
            unsafe {
                assert!(core.heap().start(Pool::Guest, spans[0].0, bytes, None));
                assert!(core.heap().start(Pool::Own, spans[1].0, bytes, None));
            }
            entry::leave(held);
            core
        }
        fn heap(&self) -> &CoreHeap {
            // SAFETY: leaked in `new`, freed only in `drop`.
            unsafe { self.heap.as_ref() }
        }
        // A call inside this core, holding its gate or not.
        fn call(&self, kind: Kind, gate: bool) -> Entry {
            entry::enter(Entry { core: ptr::null_mut(), heap: self.heap.as_ptr().cast_const().cast(), kind, gate })
        }
    }
    impl Drop for Core {
        fn drop(&mut self) {
            let held = self.call(Kind::Control, true);
            self.heap().stop(Pool::Guest);
            self.heap().stop(Pool::Own);
            entry::leave(held);
            // SAFETY: from `Box::leak` in `new`, freed once.
            drop(unsafe { Box::from_raw(self.heap.as_ptr()) });
            for (span, layout) in self.spans {
                // SAFETY: from `alloc` with this layout.
                unsafe { System.dealloc(span, layout) };
            }
        }
    }

    const L: Layout = match Layout::from_size_align(200, 8) {
        Ok(l) => l,
        Err(_) => panic!(),
    };

    #[test]
    fn outside_any_core_everything_is_the_systems() {
        let core = Core::new(CHUNK_BYTES);
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let p = RtRouter.alloc(L);
            assert_eq!(core.heap().pool_of(p), None);
            RtRouter.dealloc(p, L);
        }
    }

    #[test]
    fn a_door_by_the_gates_holder_is_served_by_its_cores_own_pool() {
        let core = Core::new(CHUNK_BYTES);
        let outside = core.call(Kind::Render, true);
        let door = Door::enter();
        // SAFETY: each pointer is freed once with the layout it was made with.
        unsafe {
            let p = RtRouter.alloc(L);
            assert_eq!(core.heap().pool_of(p), Some(Pool::Own));
            RtRouter.dealloc(p, L);
            // Same spot again: a freed block goes to the head of its size
            // class's list, so the next request of that size gets it back.
            let q = RtRouter.alloc(L);
            assert_eq!(p, q);
            RtRouter.dealloc(q, L);
            // Over-aligned too.
            let la = Layout::from_size_align(100, 64).unwrap();
            let a = RtRouter.alloc(la);
            assert!(core.heap().pool_of(a) == Some(Pool::Own) && (a as usize) % 64 == 0);
            RtRouter.dealloc(a, la);
        }
        assert_eq!(core.heap().census(Pool::Own).used, 0);
        drop(door);
        entry::leave(outside);
    }

    // Suspect 1 (docs/ENGINES.md): every sink and port door entered the
    // pool, from the guest's thread, the NRT gateway, MIDI clock out, Link
    // Audio and tracks alike, and two at once shared a single-threaded pool.
    // A pool is the gate holder's; a door on any other thread is the
    // system's.
    #[test]
    fn a_door_without_the_gate_is_served_by_the_system() {
        let core = Core::new(CHUNK_BYTES);
        let outside = core.call(Kind::Control, false);
        let door = Door::enter(); // a thread holding no core's gate: MIDI clock out's, say
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let p = RtRouter.alloc(L);
            assert_eq!(core.heap().pool_of(p), None, "a door without the gate took from the pool");
            RtRouter.dealloc(p, L);
        }
        drop(door);
        entry::leave(outside);
    }

    #[test]
    fn a_free_without_the_gate_parks_and_the_holder_takes_it_back() {
        let core = Core::new(CHUNK_BYTES);
        let held = core.call(Kind::Door, true);
        // SAFETY: a plain allocation, freed below.
        let p = unsafe { RtRouter.alloc(L) };
        entry::leave(held);
        assert_eq!(core.heap().pool_of(p), Some(Pool::Own));
        let addr = p.expose_provenance();
        std::thread::scope(|s| {
            // SAFETY: `p` is live, and this is its one free, on a thread with
            // no entry at all: the map finds the heap.
            s.spawn(|| unsafe { RtRouter.dealloc(ptr::with_exposed_provenance_mut(addr), L) });
        });
        assert!(core.heap().census(Pool::Own).used >= 200, "a free without the gate reached the pool");
        let held = core.call(Kind::Door, true);
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let q = RtRouter.alloc(L);
            assert_eq!(q, p, "the parked block comes back to the gate's holder");
            RtRouter.dealloc(q, L);
        }
        entry::leave(held);
        assert_eq!(core.heap().census(Pool::Own).used, 0);
    }

    #[test]
    fn a_guest_call_by_the_gates_holder_is_served_by_its_cores_guest_pool_and_a_door_still_wins() {
        let core = Core::new(CHUNK_BYTES);
        let outside = core.call(Kind::GuestProcessing, true);
        // SAFETY: each pointer is freed once with the layout it was made with.
        unsafe {
            let g = RtRouter.alloc(L);
            assert_eq!(core.heap().pool_of(g), Some(Pool::Guest), "an alloc inside a guest call lands in its core's guest pool");
            let la = Layout::from_size_align(100, 64).unwrap();
            let a = RtRouter.alloc(la);
            assert!(core.heap().pool_of(a) == Some(Pool::Guest) && (a as usize) % 64 == 0);
            RtRouter.dealloc(a, la);
            // A door opened inside the guest's call (a sink's) allocates from
            // the core's own pool, not the guest's.
            {
                let _e = Door::enter();
                let e = RtRouter.alloc(L);
                assert_eq!(core.heap().pool_of(e), Some(Pool::Own), "a door wins inside a guest call");
                RtRouter.dealloc(e, L);
            }
            RtRouter.dealloc(g, L);
        }
        entry::leave(outside);
        // Without the gate, a guest call's allocations are the system's.
        let outside = core.call(Kind::GuestBoot, false);
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let s = RtRouter.alloc(L);
            assert_eq!(core.heap().pool_of(s), None);
            RtRouter.dealloc(s, L);
        }
        entry::leave(outside);
    }

    #[test]
    fn a_pool_that_cannot_meet_an_allocation_falls_back_to_the_system_and_counts_it() {
        let core = Core::new(CHUNK_BYTES);
        let outside = core.call(Kind::Door, true);
        let big = Layout::from_size_align(4 * CHUNK_BYTES, 8).unwrap();
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let p = RtRouter.alloc(big);
            assert!(!p.is_null());
            assert_eq!(core.heap().pool_of(p), None);
            RtRouter.dealloc(p, big);
        }
        entry::leave(outside);
        assert_eq!(core.heap().census(Pool::Own).fallbacks, 1);
        assert_eq!(core.heap().census(Pool::Guest).fallbacks, 0);
    }

    // Two cores, one thread rendering each in turn: a block of one core's
    // pool never lands in the other's, and each takes back its own.
    #[test]
    fn two_cores_rendered_in_turn_keep_their_own_pools() {
        let (a, b) = (Core::new(CHUNK_BYTES), Core::new(CHUNK_BYTES));
        let mut held = Vec::new();
        for (core, pool) in [(&a, Pool::Own), (&b, Pool::Own), (&a, Pool::Guest), (&b, Pool::Guest)] {
            let kind = if pool == Pool::Own { Kind::Door } else { Kind::GuestProcessing };
            let outside = core.call(kind, true);
            // SAFETY: freed below with the layout it was made with.
            let p = unsafe { RtRouter.alloc(L) };
            entry::leave(outside);
            assert_eq!(core.heap().pool_of(p), Some(pool));
            held.push(p);
        }
        assert_eq!(a.heap().pool_of(held[1]), None, "core b's block is in core a's pool");
        for p in held {
            // SAFETY: each is freed once, with no entry: the map finds its heap.
            unsafe { RtRouter.dealloc(p, L) };
        }
        for core in [&a, &b] {
            let outside = core.call(Kind::Door, true);
            // SAFETY: freed once, by the holder, which takes back what parked.
            unsafe {
                let p = RtRouter.alloc(L);
                RtRouter.dealloc(p, L);
            }
            entry::leave(outside);
            assert_eq!(core.heap().census(Pool::Own).used, 0);
        }
    }
}
