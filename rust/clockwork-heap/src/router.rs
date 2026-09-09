// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The RT router — a global allocator that keeps the engine inside its arena.
//!
//! The law this enforces (Sam, 2026-08-21): **no locks of any kind on the
//! audio thread, and no malloc except our own arena management.** The render
//! path already allocates nothing; this covers the control path — `/d_recv`,
//! `/s_new`, `/n_set` all execute on the audio thread in every deployment of
//! this engine, worklet and native alike, and their `HashMap`s and `Vec`s
//! used to reach whatever the platform's malloc was.
//!
//! Mechanism: at boot (control time, before the first render) the host arms
//! the router with one contiguous region claimed from the system allocator —
//! that claim is the "arena management" the law permits. From then on, any
//! allocation made while an engine entry point is on the stack (a
//! thread-local scope flag, set by the FFI layer) is served from the region
//! by the same segregated-fit pool the buffer heap uses, in constant time
//! whatever the pool already holds. Routing on free is a two-compare range
//! check. No atomics are contended, and the pool is touched by exactly one
//! thread — the engine's. (Arming itself may briefly spin, but that is at
//! boot, off the audio thread, and never once the router is armed.)
//!
//! Edges, stated rather than hidden:
//!
//! * **Foreign-thread frees** of arena memory (native: a loader thread
//!   dropping something the engine built) push the block onto a Treiber
//!   stack — the freed block's own bytes hold the link, so the push
//!   allocates nothing — and the engine reclaims the stack at its next
//!   entry. Lock-free; the audio thread never waits on it.
//! * **Exhaustion** falls back to the system allocator and *counts* it
//!   (`fallback_allocs`), because a dropout beats an abort mid-performance.
//!   The census asserts the counter stays zero under test load; a nonzero
//!   count in production is a sizing bug made visible, not a silent lie.
//! * **Over-aligned requests** (align > 16) are served from the region with
//!   a back-pointer stashed below the aligned address.
//! * Unarmed, or outside engine scope, everything passes through to the
//!   system allocator untouched — `cargo test` binaries and foreign threads
//!   see the allocator they always had.

#[allow(unused_imports)]
use alloc::{borrow::ToOwned, boxed::Box, format, string::{String, ToString}, vec, vec::Vec};

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::ptr;
use std::sync::atomic::{AtomicPtr, AtomicU32, AtomicUsize, Ordering};

use crate::HeapPool;

const POOL_ALIGN: usize = 16;

/// The armed region, held as a real pointer so it stays a reachable root
/// (Miri's leak checker agrees the process-lifetime arena is not a leak) and
/// keeps its provenance.
static REGION: AtomicPtr<u8> = AtomicPtr::new(ptr::null_mut());
static REGION_LEN: AtomicUsize = AtomicUsize::new(0);
static POOL: AtomicPtr<HeapPool> = AtomicPtr::new(ptr::null_mut());
/// The arming state machine: 0 unarmed, 1 arming, 2 armed. One variable
/// rather than two, so a waiter's conclusion rests on a single release/
/// acquire pair and not on reasoning across `ARMING` and `POOL`. An earlier
/// version used a flag plus `armed()` and Miri found the hole in it.
///
/// `arm` promises that subsequent calls are ignored, which is only true if a
/// caller that finds arming already under way *waits* for it: returning early
/// would hand back an unarmed router, and the very next engine-scope
/// allocation would pass through to the system allocator and never land in
/// the region.
static ARMING: AtomicU32 = AtomicU32::new(0);
const ARM_IDLE: u32 = 0;
const ARM_BUSY: u32 = 1;
const ARM_DONE: u32 = 2;
static FALLBACK_ALLOCS: AtomicU32 = AtomicU32::new(0);
/// Live bytes currently served from the arena (payload sizes, engine view).
static USED_BYTES: AtomicUsize = AtomicUsize::new(0);
static ARENA_ALLOCS: AtomicU32 = AtomicU32::new(0);
static FOREIGN_FREES: AtomicU32 = AtomicU32::new(0);
/// In-scope frees that had to reach the system allocator (pre-arm pointers).
static SCOPED_SYSTEM_FREES: AtomicU32 = AtomicU32::new(0);
/// Cross-thread frees of arena blocks, parked until the engine reclaims.
static DEFERRED: AtomicPtr<Deferred> = AtomicPtr::new(ptr::null_mut());

struct Deferred {
    next: *mut Deferred,
}

thread_local! {
    /// Depth of engine FFI entries on this thread's stack.
    static SCOPE: Cell<u32> = const { Cell::new(0) };
}

/// RAII: the FFI layer holds one of these across every engine entry point.
pub struct EngineScope;

impl EngineScope {
    #[inline]
    pub fn enter() -> EngineScope {
        SCOPE.with(|s| s.set(s.get() + 1));
        // Entering the engine is the natural boundary to reclaim frees that
        // arrived from other threads while it was away.
        drain_deferred();
        EngineScope
    }
}

impl Drop for EngineScope {
    #[inline]
    fn drop(&mut self) {
        SCOPE.with(|s| s.set(s.get() - 1));
    }
}

#[inline]
fn in_scope() -> bool {
    SCOPE.with(|s| s.get() > 0)
}

#[inline]
fn in_region(p: *mut u8) -> bool {
    // Relaxed: the region is set once at arm time, before any engine-scope
    // allocation can exist, and never moves.
    let base = REGION.load(Ordering::Relaxed);
    if base.is_null() {
        return false;
    }
    let len = REGION_LEN.load(Ordering::Relaxed);
    (p as usize).wrapping_sub(base as usize) < len
}

fn drain_deferred() {
    let mut node = DEFERRED.swap(ptr::null_mut(), Ordering::Acquire);
    if node.is_null() {
        return;
    }
    let pool = POOL.load(Ordering::Relaxed);
    while !node.is_null() {
        // SAFETY: each node is an arena block parked by `dealloc`; its link
        // was written before the release-publish that got it here, and the
        // pool is armed because only arena blocks are ever parked.
        unsafe {
            let next = (*node).next;
            (*pool).free(node.cast::<u8>());
            node = next;
        }
    }
}

/// One-shot region handoff to the pool's area callback.
static PENDING_AREA: AtomicPtr<std::os::raw::c_void> = AtomicPtr::new(ptr::null_mut());

unsafe extern "C" fn router_new_area(_size: usize) -> *mut std::os::raw::c_void {
    PENDING_AREA.swap(ptr::null_mut(), Ordering::Relaxed)
}

unsafe extern "C" fn router_free_area(_p: *mut std::os::raw::c_void) {
    // The region lives for the process; free_all keeps the initial area.
}

/// Claim the region and arm the router. Called once at boot, from control
/// time — this is the one system allocation the law names as ours.
/// Subsequent calls are ignored (worklet re-init reuses the armed arena;
/// its pool survives engine restarts because restarts free their sessions
/// through the normal paths).
pub fn arm(bytes: usize) {
    let Some(mut claim) = ArmClaim::take() else { return };
    let size = bytes.max(64 * 1024);
    let layout = Layout::from_size_align(size, POOL_ALIGN).unwrap();
    // SAFETY: a non-zero size; the region is never freed, so no layout has
    // to be remembered.
    let region = unsafe { System.alloc(layout) };
    if region.is_null() {
        return; // unarmed router = passthrough; visible via armed()
    }
    // SAFETY: fresh from the system allocator with exactly this size and
    // alignment.
    unsafe { arm_region(region, size) };
    claim.done();
}

/// Arm from a region the host already owns (wasm: a slice of the SAB's
/// reserved RT pool). No system allocation at all; the region must outlive
/// the process's engine use and is never freed here.
///
/// # Safety
/// `base..base+bytes` must be valid, 16-aligned, unused memory.
pub unsafe fn arm_at(base: *mut u8, bytes: usize) {
    let Some(mut claim) = ArmClaim::take() else { return };
    if !base.is_null() && bytes >= 64 * 1024 {
        // SAFETY: the caller's contract is `arm_region`'s.
        unsafe { arm_region(base, bytes) };
        claim.done();
    }
}

/// The right to arm, held for the duration of one attempt.
///
/// `take` returns `None` when the arena is already armed. A caller that loses
/// the race blocks until the winner is finished and then re-reads the state,
/// so no thread ever proceeds on the assumption that an unarmed router means
/// "no arena". Arming happens once, at boot, off the audio thread, and the
/// wait is over in the time it takes to make one system allocation.
///
/// The claim publishes its outcome on drop: `ARM_DONE` when the caller says
/// it armed, `ARM_IDLE` when it could not, so a failed attempt can be retried
/// rather than wedging every later caller.
struct ArmClaim {
    armed: bool,
}

impl ArmClaim {
    fn take() -> Option<ArmClaim> {
        loop {
            match ARMING.compare_exchange(
                ARM_IDLE,
                ARM_BUSY,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Some(ArmClaim { armed: false }),
                Err(ARM_DONE) => return None,
                // Someone else is arming: wait for them to publish, then look
                // again — they may have failed, leaving the work to us.
                Err(_) => {
                    while ARMING.load(Ordering::Acquire) == ARM_BUSY {
                        core::hint::spin_loop();
                    }
                }
            }
        }
    }

    /// The arena is armed; every later caller may return immediately.
    fn done(&mut self) {
        self.armed = true;
    }
}

impl Drop for ArmClaim {
    fn drop(&mut self) {
        // Release: whatever `arm_region` wrote must be visible to whoever
        // observes ARM_DONE.
        ARMING.store(
            if self.armed { ARM_DONE } else { ARM_IDLE },
            Ordering::Release,
        );
    }
}

/// Build the pool over `region` and publish it.
///
/// # Safety
/// `region..region+size` must be valid, 16-aligned memory that nothing else
/// uses for the rest of the process.
unsafe fn arm_region(region: *mut u8, size: usize) {
    PENDING_AREA.store(region.cast::<std::os::raw::c_void>(), Ordering::Relaxed);
    let pool = Box::into_raw(Box::new(HeapPool::new(
        Some(router_new_area),
        Some(router_free_area),
        size,
        0, // growth 0: the pool may never ask the system for more mid-run
    )));
    REGION.store(region, Ordering::Relaxed);
    REGION_LEN.store(size, Ordering::Relaxed);
    POOL.store(pool, Ordering::Release);
}

pub fn armed() -> bool {
    !POOL.load(Ordering::Relaxed).is_null()
}

pub fn fallback_allocs() -> u32 {
    FALLBACK_ALLOCS.load(Ordering::Relaxed)
}

pub fn foreign_frees() -> u32 {
    FOREIGN_FREES.load(Ordering::Relaxed)
}

pub fn used_bytes() -> usize {
    USED_BYTES.load(Ordering::Relaxed)
}

pub fn arena_allocs() -> u32 {
    ARENA_ALLOCS.load(Ordering::Relaxed)
}

pub fn scoped_system_frees() -> u32 {
    SCOPED_SYSTEM_FREES.load(Ordering::Relaxed)
}

/// The engine's allocator: arena inside engine scope, system everywhere else.
pub struct RtRouter;

// SAFETY: every pointer handed out is either the system allocator's or a
// pool payload of at least `layout.size()` bytes at `layout.align()`, and
// `dealloc` routes each back to the allocator it came from by the region
// check. The pool is only ever touched from inside engine scope, which is
// one thread by the engine's own rule, and foreign frees are parked on a
// lock-free stack rather than reaching it.
unsafe impl GlobalAlloc for RtRouter {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pool = POOL.load(Ordering::Acquire);
        if pool.is_null() || !in_scope() {
            // SAFETY: the caller's contract is `System`'s.
            return unsafe { System.alloc(layout) };
        }
        // SAFETY: `pool` is armed and this thread is inside engine scope,
        // the only place it is used. The over-aligned branch asked for
        // `align + POOL_ALIGN` bytes of slack, so the aligned address and
        // the word below it both lie inside the payload.
        let p = unsafe {
            if layout.align() <= POOL_ALIGN {
                (*pool).alloc(layout.size())
            } else {
                // Over-aligned: claim slack, align up, stash the raw pointer
                // in the word below the aligned address for dealloc to find.
                let raw = (*pool).alloc(layout.size() + layout.align() + POOL_ALIGN);
                if raw.is_null() {
                    ptr::null_mut()
                } else {
                    let aligned = ((raw as usize + POOL_ALIGN + layout.align() - 1)
                        & !(layout.align() - 1)) as *mut u8;
                    *aligned.cast::<usize>().sub(1) = raw as usize;
                    aligned
                }
            }
        };
        if p.is_null() {
            // Exhausted: counted, never silent. A dropout risk beats an abort.
            FALLBACK_ALLOCS.fetch_add(1, Ordering::Relaxed);
            // SAFETY: the caller's contract is `System`'s.
            return unsafe { System.alloc(layout) };
        }
        USED_BYTES.fetch_add(layout.size(), Ordering::Relaxed);
        ARENA_ALLOCS.fetch_add(1, Ordering::Relaxed);
        p
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        if !in_region(p) {
            if in_scope() && armed() {
                // A pre-arm (or fallback) pointer dying inside engine scope:
                // legal only as a boot leftover, so it is counted, and the
                // law test holds the count still.
                SCOPED_SYSTEM_FREES.fetch_add(1, Ordering::Relaxed);
            }
            // SAFETY: not in the region, so `alloc` got it from `System`
            // with this layout.
            return unsafe { System.dealloc(p, layout) };
        }
        USED_BYTES.fetch_sub(layout.size(), Ordering::Relaxed);
        let block = if layout.align() <= POOL_ALIGN {
            p
        } else {
            // SAFETY: `alloc` stashed the raw payload pointer in the word
            // below every over-aligned address it handed out.
            unsafe { (*p.cast::<usize>().sub(1)) as *mut u8 }
        };
        if in_scope() {
            let pool = POOL.load(Ordering::Relaxed);
            // SAFETY: `block` is a payload of the armed pool, and this thread
            // is inside engine scope.
            unsafe { (*pool).free(block) };
        } else {
            // A foreign thread returning arena memory: park it on the
            // deferred stack — the block's own bytes carry the link — for
            // the engine to reclaim at its next entry.
            FOREIGN_FREES.fetch_add(1, Ordering::Relaxed);
            let node = block.cast::<Deferred>();
            let mut head = DEFERRED.load(Ordering::Relaxed);
            loop {
                // SAFETY: the block is dead to its owner and at least 16
                // bytes wide, so its first word is ours for the link.
                unsafe { (*node).next = head };
                match DEFERRED.compare_exchange_weak(
                    head,
                    node,
                    Ordering::Release,
                    Ordering::Relaxed,
                ) {
                    Ok(_) => break,
                    Err(h) => head = h,
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The suite shares one process; arm once, small.
    fn armed_router() -> RtRouter {
        arm(256 * 1024);
        RtRouter
    }

    // The pool is one engine thread's by contract (`HeapPool::alloc` takes
    // `&mut`), and the test harness runs cases on parallel threads, each of
    // which enters engine scope on its own thread. Overlapping cases were
    // therefore racing on the pool — a corrupted free list showed up as an
    // allocation that missed the region, or a reclaimed block that was not
    // the one expected — so every case that touches the arena holds this.
    static ENGINE: std::sync::Mutex<()> = std::sync::Mutex::new(());
    fn engine() -> std::sync::MutexGuard<'static, ()> {
        ENGINE.lock().unwrap_or_else(|e| e.into_inner())
    }

    #[test]
    fn out_of_scope_passes_through() {
        let _engine = engine();
        let r = armed_router();
        // SAFETY: a layout the system allocator accepts, freed once.
        unsafe {
            let l = Layout::from_size_align(64, 8).unwrap();
            let p = r.alloc(l);
            assert!(!in_region(p), "unscoped alloc must not touch the arena");
            r.dealloc(p, l);
        }
    }

    #[test]
    fn scoped_allocs_land_in_region_and_free_back() {
        let _engine = engine();
        let r = armed_router();
        let _g = EngineScope::enter();
        // SAFETY: each pointer is freed once with the layout it was made with.
        unsafe {
            let l = Layout::from_size_align(200, 8).unwrap();
            let p = r.alloc(l);
            assert!(in_region(p));
            r.dealloc(p, l);
            // Same spot again: a freed block goes to the head of its size
            // class's list, so the next request of that size gets it back.
            let q = r.alloc(l);
            assert_eq!(p, q);
            r.dealloc(q, l);
        }
    }

    #[test]
    fn over_aligned_round_trips() {
        let _engine = engine();
        let r = armed_router();
        let _g = EngineScope::enter();
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let l = Layout::from_size_align(100, 64).unwrap();
            let p = r.alloc(l);
            assert!(in_region(p));
            assert_eq!(p as usize % 64, 0);
            r.dealloc(p, l);
        }
    }

    #[test]
    fn foreign_free_parks_then_engine_reclaims() {
        let _engine = engine();
        let r = armed_router();
        let (p, l) = {
            let _g = EngineScope::enter();
            let l = Layout::from_size_align(300, 8).unwrap();
            // SAFETY: a plain allocation, freed below.
            (unsafe { r.alloc(l) }, l)
        };
        assert!(in_region(p));
        let before = foreign_frees();
        // SAFETY: `p` is live, and this is its one free.
        unsafe { r.dealloc(p, l) }; // out of scope: parks, does not free
        assert_eq!(foreign_frees(), before + 1);
        {
            let _g = EngineScope::enter(); // entry drains the deferred stack
            // SAFETY: freed once with the layout it was made with.
            unsafe {
                let q = r.alloc(l);
                assert_eq!(q, p, "reclaimed block is reusable");
                r.dealloc(q, l);
            }
        }
    }

    #[test]
    fn concurrent_arming_arms_exactly_once() {
        let _engine = engine();
        // The bug this pins: `arm` used to check POOL and then act, so every
        // thread that got past the null check allocated its OWN region and
        // called arm_region, each overwriting REGION. A block handed out from
        // the first pool then sat outside the region the last writer named,
        // so `in_region` said false for memory the arena really owned — and
        // every region but the last leaked.
        //
        // Only the arming races here. The pool itself is single-engine-thread
        // by contract — `HeapPool::alloc` takes `&mut` — so allocating from
        // sixteen threads at once would be `&mut` aliasing, which is what an
        // earlier draft of this test did until Miri said so.
        //
        // Spawn latency alone is longer than an arm takes, so without a
        // rendezvous the first thread is finished before the second starts
        // and nothing contends. The barrier is what makes the race reachable.
        let gate = std::sync::Arc::new(std::sync::Barrier::new(16));
        let seen: Vec<usize> = (0..16)
            .map(|_| {
                let gate = std::sync::Arc::clone(&gate);
                std::thread::spawn(move || {
                    gate.wait();
                    let _r = armed_router();
                    assert!(armed(), "arm returned with the router still unarmed");
                    REGION.load(Ordering::Relaxed) as usize
                })
            })
            .collect::<Vec<_>>()
            .into_iter()
            .map(|t| t.join().expect("a thread saw an unarmed router"))
            .collect();

        assert!(seen[0] != 0, "nobody armed");
        let mut distinct = seen.clone();
        distinct.sort_unstable();
        distinct.dedup();
        assert_eq!(
            distinct.len(),
            1,
            "the region moved under a live allocation: {} distinct regions",
            distinct.len()
        );

        // And the arena the winner armed actually serves the engine thread.
        let r = armed_router();
        let _g = EngineScope::enter();
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let l = Layout::from_size_align(128, 8).unwrap();
            let p = r.alloc(l);
            assert!(in_region(p), "scoped alloc escaped the arena");
            r.dealloc(p, l);
        }
    }

    #[test]
    fn exhaustion_falls_back_and_counts() {
        let _engine = engine();
        let r = armed_router();
        let _g = EngineScope::enter();
        // SAFETY: freed once with the layout it was made with.
        unsafe {
            let l = Layout::from_size_align(10 * 1024 * 1024, 8).unwrap();
            let before = fallback_allocs();
            let p = r.alloc(l); // larger than the whole test arena
            assert!(!p.is_null());
            assert!(!in_region(p));
            assert_eq!(fallback_allocs(), before + 1);
            r.dealloc(p, l);
        }
    }
}
