// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! The RT router — a global allocator that keeps the engine inside its arena.
//!
//! The rule this enforces: **no locks of any kind on the audio thread, and
//! no malloc except our own arena management.** The render path allocates
//! nothing; this covers the control path — `/d_recv`, `/s_new`, `/n_set`
//! all execute on the audio thread in every deployment of this engine,
//! worklet and native alike, and their `HashMap`s and `Vec`s would otherwise
//! reach the platform's malloc.
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
//!
//! **The guest's route.** A guest written in Rust allocates the Rust way,
//! and its `Box`es and `Vec`s must land in the guest's own memory, not in
//! malloc and not in this router's block (which is clockwork's). So the
//! host installs three hooks at boot (`clockwork_router_guest_hooks`: take,
//! give back, and "is this yours") over the pool it runs on the guest's
//! bulk memory (src/clockwork_pool.cpp), and opens a *guest scope* around
//! every guest entry point (`clockwork_router_guest_enter` / `_exit`). An
//! allocation made inside a guest scope and outside an engine scope goes
//! through the take hook; a free of anything the pool holds goes through
//! the give-back hook, from any thread (the pool parks a foreign thread's
//! free itself). Exhaustion falls back to the system allocator and counts
//! it (`guest_fallback_allocs`), as the engine's own route does. The guest
//! never learns any of this: it writes ordinary Rust.

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
/// acquire pair and not on reasoning across `ARMING` and `POOL`.
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

/// The guest's route: the host's hooks over the guest's pool, and how many
/// in-scope allocations the pool could not meet.
type GuestTake = unsafe extern "C" fn(usize) -> *mut std::os::raw::c_void;
type GuestGive = unsafe extern "C" fn(*mut std::os::raw::c_void);
type GuestHolds = unsafe extern "C" fn(*const std::os::raw::c_void) -> std::os::raw::c_int;
static GUEST_TAKE: AtomicPtr<()> = AtomicPtr::new(ptr::null_mut());
static GUEST_GIVE: AtomicPtr<()> = AtomicPtr::new(ptr::null_mut());
static GUEST_HOLDS: AtomicPtr<()> = AtomicPtr::new(ptr::null_mut());
static GUEST_FALLBACK_ALLOCS: AtomicU32 = AtomicU32::new(0);

thread_local! {
    /// Depth of engine FFI entries on this thread's stack.
    static SCOPE: Cell<u32> = const { Cell::new(0) };
    /// Depth of guest entry points on this thread's stack.
    static GUEST_SCOPE: Cell<u32> = const { Cell::new(0) };
}

/// Install the host's hooks over the guest's pool. NULL hooks remove the
/// route (teardown). Called at boot and teardown, on the control thread,
/// never while a guest scope is open.
///
/// # Safety
/// The hooks must behave as described in the module doc, and `take` must
/// return 16-aligned payloads of at least the bytes asked for.
#[no_mangle]
pub unsafe extern "C" fn clockwork_router_guest_hooks(
    take: Option<GuestTake>,
    give: Option<GuestGive>,
    holds: Option<GuestHolds>,
) {
    GUEST_TAKE.store(take.map_or(ptr::null_mut(), |f| f as *mut ()), Ordering::Release);
    GUEST_GIVE.store(give.map_or(ptr::null_mut(), |f| f as *mut ()), Ordering::Release);
    GUEST_HOLDS.store(holds.map_or(ptr::null_mut(), |f| f as *mut ()), Ordering::Release);
}

/// A guest entry point is on this thread's stack: allocations go to the
/// guest's pool until the matching exit.
#[no_mangle]
pub extern "C" fn clockwork_router_guest_enter() {
    GUEST_SCOPE.with(|s| s.set(s.get() + 1));
}

#[no_mangle]
pub extern "C" fn clockwork_router_guest_exit() {
    GUEST_SCOPE.with(|s| s.set(s.get().saturating_sub(1)));
}

/// Allocations inside a guest scope the guest's pool could not meet, served
/// by the system allocator instead: a sizing bug made visible.
#[no_mangle]
pub extern "C" fn clockwork_router_guest_fallback_allocs() -> u32 {
    GUEST_FALLBACK_ALLOCS.load(Ordering::Relaxed)
}

#[inline]
fn in_guest_scope() -> bool {
    GUEST_SCOPE.with(|s| s.get() > 0)
}

#[inline]
fn guest_hooks() -> Option<(GuestTake, GuestGive, GuestHolds)> {
    let take = GUEST_TAKE.load(Ordering::Acquire);
    let give = GUEST_GIVE.load(Ordering::Acquire);
    let holds = GUEST_HOLDS.load(Ordering::Acquire);
    if take.is_null() || give.is_null() || holds.is_null() {
        return None;
    }
    // SAFETY: the pointers were made from these exact function types in
    // `clockwork_router_guest_hooks` and are only ever read back as them.
    unsafe {
        Some((
            core::mem::transmute::<*mut (), GuestTake>(take),
            core::mem::transmute::<*mut (), GuestGive>(give),
            core::mem::transmute::<*mut (), GuestHolds>(holds),
        ))
    }
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

unsafe extern "C" fn router_new_area(_ctx: *mut std::os::raw::c_void, _size: usize) -> *mut std::os::raw::c_void {
    PENDING_AREA.swap(ptr::null_mut(), Ordering::Relaxed)
}

unsafe extern "C" fn router_free_area(_ctx: *mut std::os::raw::c_void, _p: *mut std::os::raw::c_void) {
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

/// `arm_at` for the C++ host: arm from `base..base+bytes`, a block the host
/// took once for the life of the process (clockwork_heap.cpp, at heap init).
/// Returns 1 once armed — by this call or an earlier one — and 0 when the
/// router is still unarmed (a null base, or fewer than 64 KB).
///
/// # Safety
/// `arm_at`'s contract.
#[no_mangle]
pub unsafe extern "C" fn clockwork_router_arm(base: *mut u8, bytes: usize) -> std::os::raw::c_int {
    // SAFETY: the caller's contract is `arm_at`'s.
    unsafe { arm_at(base, bytes) };
    armed() as std::os::raw::c_int
}

/// How much of the armed region is in use, for the host's census: live
/// payload bytes, and how many allocations fell back to the system.
#[no_mangle]
pub extern "C" fn clockwork_router_used_bytes() -> usize { used_bytes() }
#[no_mangle]
pub extern "C" fn clockwork_router_fallback_allocs() -> u32 { fallback_allocs() }

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
        ptr::null_mut(),
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
        // The guest's route: inside a guest entry point and outside any
        // engine entry point it opened (a sink's door, say, which is
        // clockwork's and allocates from clockwork's block).
        if !in_scope() && in_guest_scope() {
            if let Some((take, _, _)) = guest_hooks() {
                // SAFETY: the hook's contract is the module doc's; the
                // over-aligned branch asked for slack as the engine route
                // below does.
                let p = unsafe {
                    if layout.align() <= POOL_ALIGN {
                        take(layout.size()).cast::<u8>()
                    } else {
                        let raw = take(layout.size() + layout.align() + POOL_ALIGN).cast::<u8>();
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
                if !p.is_null() {
                    return p;
                }
                GUEST_FALLBACK_ALLOCS.fetch_add(1, Ordering::Relaxed);
            }
            // SAFETY: the caller's contract is `System`'s.
            return unsafe { System.alloc(layout) };
        }
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
        // The guest's pool, from any thread and any scope: the pool routes a
        // foreign thread's free itself.
        if let Some((_, give, holds)) = guest_hooks() {
            // SAFETY: the hook only reads the address.
            if unsafe { holds(p.cast::<std::os::raw::c_void>()) } != 0 {
                let block = if layout.align() <= POOL_ALIGN {
                    p
                } else {
                    // SAFETY: `alloc` stashed the raw payload pointer in the
                    // word below every over-aligned address it handed out.
                    unsafe { (*p.cast::<usize>().sub(1)) as *mut u8 }
                };
                // SAFETY: a payload the pool holds, dead to its owner.
                unsafe { give(block.cast::<std::os::raw::c_void>()) };
                return;
            }
        }
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
    // which enters engine scope on its own thread. Overlapping cases would
    // race on the pool and corrupt its free list, so every case that touches
    // the arena holds this.
    static ENGINE: std::sync::Mutex<()> = std::sync::Mutex::new(());
    fn engine() -> std::sync::MutexGuard<'static, ()> {
        ENGINE.lock().unwrap_or_else(|e| e.into_inner())
    }

    // The guest's route: a host's hooks over a span, installed and removed.
    static GUEST_SPAN: std::sync::Mutex<Option<(usize, usize, Vec<usize>)>> = std::sync::Mutex::new(None);
    unsafe extern "C" fn guest_take(bytes: usize) -> *mut std::os::raw::c_void {
        // A bump allocator over the span: enough to prove the route.
        let mut g = GUEST_SPAN.lock().unwrap();
        let (base, len, taken) = g.as_mut().unwrap();
        let at = taken.iter().sum::<usize>();
        let size = (bytes + 15) & !15;
        if at + size > *len { return ptr::null_mut(); }
        taken.push(size);
        (*base + at) as *mut std::os::raw::c_void
    }
    unsafe extern "C" fn guest_give(_p: *mut std::os::raw::c_void) {}
    unsafe extern "C" fn guest_holds(p: *const std::os::raw::c_void) -> std::os::raw::c_int {
        let g = GUEST_SPAN.lock().unwrap();
        let Some((base, len, _)) = g.as_ref() else { return 0 };
        ((p as usize).wrapping_sub(*base) < *len) as std::os::raw::c_int
    }

    #[test]
    fn guest_scope_allocs_land_in_the_guests_span_and_engine_scope_still_wins() {
        let _engine = engine();
        let r = armed_router();
        let span = vec![0u8; 64 * 1024 + 16];
        let base = (span.as_ptr() as usize + 15) & !15;
        *GUEST_SPAN.lock().unwrap() = Some((base, 64 * 1024, Vec::new()));
        // SAFETY: the hooks above behave as the module doc asks.
        unsafe { clockwork_router_guest_hooks(Some(guest_take), Some(guest_give), Some(guest_holds)) };
        // SAFETY: each pointer is freed once with the layout it was made with.
        unsafe {
            let l = Layout::from_size_align(200, 8).unwrap();
            // Outside any scope: the system's, as before.
            let p = r.alloc(l);
            assert!(!in_region(p) && guest_holds(p.cast()) == 0);
            r.dealloc(p, l);
            // Inside a guest scope: the guest's span.
            clockwork_router_guest_enter();
            let g = r.alloc(l);
            assert!(guest_holds(g.cast()) != 0, "a guest-scope alloc lands in the guest's span");
            // Over-aligned too.
            let la = Layout::from_size_align(100, 64).unwrap();
            let a = r.alloc(la);
            assert!(guest_holds(a.cast()) != 0 && (a as usize) % 64 == 0);
            r.dealloc(a, la);
            // An engine entry point opened inside the guest's (a sink's door)
            // allocates from the engine's block, not the guest's.
            {
                let _e = EngineScope::enter();
                let e = r.alloc(l);
                assert!(in_region(e), "engine scope wins inside a guest scope");
                r.dealloc(e, l);
            }
            // The span full: the system serves it, counted.
            let before = clockwork_router_guest_fallback_allocs();
            let big = Layout::from_size_align(128 * 1024, 8).unwrap();
            let f = r.alloc(big);
            assert!(!f.is_null() && guest_holds(f.cast()) == 0);
            assert_eq!(clockwork_router_guest_fallback_allocs(), before + 1);
            r.dealloc(f, big);
            clockwork_router_guest_exit();
            // Freed outside the scope: still routed to the guest's pool by
            // address, never to the system allocator.
            r.dealloc(g, l);
            clockwork_router_guest_hooks(None, None, None);
            // Hooks gone: the route is gone with them.
            clockwork_router_guest_enter();
            let s = r.alloc(l);
            assert!(guest_holds(s.cast()) == 0);
            r.dealloc(s, l);
            clockwork_router_guest_exit();
        }
        *GUEST_SPAN.lock().unwrap() = None;
        drop(span);
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
        // What this pins: an `arm` that checks POOL and then acts lets every
        // thread past the null check allocate its own region and call
        // arm_region, each overwriting REGION. A block handed out from the
        // first pool then sits outside the region the last writer named, so
        // `in_region` says false for memory the arena really owns, and every
        // region but the last leaks.
        //
        // Only the arming races here. The pool itself is single-engine-thread
        // by contract — `HeapPool::alloc` takes `&mut` — so allocating from
        // sixteen threads at once would be `&mut` aliasing.
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
