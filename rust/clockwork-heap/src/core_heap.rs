// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A core's heap (docs/ENGINES.md): two pools,
//! each clockwork's allocator run over one span of the core's memory.
//!
//! - **The guest's**, over the guest's bulk span: everything the guest
//!   allocates as it runs — a C guest's through `DspHost::alloc_bytes`, a
//!   Rust guest's every `Box` and `Vec` through the global allocator
//!   ([`crate::router`]).
//! - **The core's own**, over a block the core takes at its first init:
//!   what clockwork's Rust subsystems make inside a door (a sink's or a
//!   port's queues).
//!
//! Two, not one: the guest may touch its own span and must not reach a
//! sink's queue, and a guest that runs out must not starve clockwork's
//! queues, or the reverse.
//!
//! A pool is touched only by the thread holding the core's gate (the
//! thread's entry says so, [`crate::entry`]). A block that any other thread
//! gives back is parked on a lock-free stack — the block's own bytes carry
//! the link — and the gate's holder takes the parked blocks back at its next
//! allocation or free. While a pool runs, its span is in the address map
//! ([`crate::map`]), so a free on any thread finds the heap from the block's
//! address alone, and the heap checks the exact span. A pool never grows:
//! what it cannot meet answers null, and the router counts it.
//!
//! Nothing outlives its core. The core's own pool goes when the core does,
//! after its sinks and ports, and must be empty then: what is still in it
//! is counted ([`outlived`]), the tests require none, and its span is kept
//! rather than given back under it.
//!
//! Also here, the core's parked system frees: memory a thread without the
//! gate took from the host's own allocator for the guest, given back during
//! a block, where freeing it would be a call into the system allocator on
//! the audio thread. Parked, and released by the next call into the core
//! that is not a block, with the host's own free.

use clockwork_fence::no_unwind;
use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicPtr, AtomicU32, AtomicUsize, Ordering};

use crate::entry;
use crate::map::{self, MAP};
use crate::{HeapPool, PoolCheck};

/// Which of a core's two pools.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Pool {
    Guest,
    Own,
}

impl Pool {
    fn other(self) -> Pool {
        match self {
            Pool::Guest => Pool::Own,
            Pool::Own => Pool::Guest,
        }
    }
}

/// What gives a span back to whoever lent it.
pub type GiveBack = unsafe extern "C" fn(span: *mut c_void);

/// One core's heap. Lives as long as its core, at one address: the map and
/// every thread's entry name it.
pub struct CoreHeap {
    guest: SpanPool,
    own: SpanPool,
    /// Memory from the host's allocator, freed during a block: released off
    /// the audio thread.
    system: Parked,
}

/// A pool over one span the core was given.
struct SpanPool {
    /// The gate holder's alone.
    pool: UnsafeCell<Option<HeapPool>>,
    /// What gives the span back when the core goes, or none when its lender
    /// keeps it. Written by the gate's holder as the pool starts.
    give_back: UnsafeCell<Option<GiveBack>>,
    /// The span, read by any thread: not null while the pool runs.
    base: AtomicPtr<u8>,
    bytes: AtomicUsize,
    /// Its blocks given back without the gate.
    parked: Parked,
    /// Allocations it could not meet, which the system served.
    fallbacks: AtomicU32,
}

/// Cores whose own pool still held something when they went: clockwork
/// memory that outlived its core.
static OUTLIVED: AtomicU32 = AtomicU32::new(0);

/// Frees of blocks that outlived their core: dropped, since the span is
/// kept and no pool owns it any more, and counted.
static OUTLIVED_FREES: AtomicU32 = AtomicU32::new(0);

/// What the map names as the owner of a span kept after its core went: a
/// block freed there is nobody's to free (the router drops and counts it).
static TOMBSTONE: u8 = 0;

pub(crate) fn tombstone() -> NonNull<c_void> {
    NonNull::from(&TOMBSTONE).cast()
}

/// A free the router found in a kept span: dropped, and counted here.
pub(crate) fn note_outlived_free() {
    OUTLIVED_FREES.fetch_add(1, Ordering::Relaxed);
}

/// How many cores went with clockwork memory still in their own pool. The
/// tests require none.
pub fn outlived() -> u32 {
    OUTLIVED.load(Ordering::Relaxed)
}

/// How many blocks of a core's own pool were freed after the core went.
pub fn outlived_frees() -> u32 {
    OUTLIVED_FREES.load(Ordering::Relaxed)
}

// SAFETY: a pool is reached only by the thread holding the core's gate, one
// thread at a time (`holds_gate`), or by its drop, which has the heap alone;
// everything else is atomic.
unsafe impl Sync for CoreHeap {}
// SAFETY: as above; nothing in it is tied to a thread.
unsafe impl Send for CoreHeap {}

impl Default for CoreHeap {
    fn default() -> CoreHeap {
        CoreHeap::new()
    }
}

impl SpanPool {
    const fn new() -> SpanPool {
        SpanPool {
            pool: UnsafeCell::new(None),
            give_back: UnsafeCell::new(None),
            base: AtomicPtr::new(ptr::null_mut()),
            bytes: AtomicUsize::new(0),
            parked: Parked::new(),
            fallbacks: AtomicU32::new(0),
        }
    }

    /// Whether the span holds `p`, read whole: `start` stores the length
    /// before the base and `stop` nulls the base before the length, so a base
    /// read the same on both sides of the length is the length's base.
    fn holds(&self, p: *const u8) -> bool {
        let base = self.base.load(Ordering::Acquire);
        if base.is_null() {
            return false;
        }
        let bytes = self.bytes.load(Ordering::Acquire);
        self.base.load(Ordering::Acquire) == base && (p as usize).wrapping_sub(base as usize) < bytes
    }

    /// Free what was parked into the pool.
    fn drain(pool: &mut HeapPool, parked: &Parked) {
        let mut node = parked.take();
        while !node.is_null() {
            // SAFETY: each node is a block of this pool parked by `give`,
            // its link written before it was published.
            unsafe {
                let next = (*node).next;
                pool.free(node.cast());
                node = next;
            }
        }
    }
}

impl CoreHeap {
    pub const fn new() -> CoreHeap {
        CoreHeap { guest: SpanPool::new(), own: SpanPool::new(), system: Parked::new() }
    }

    fn of(&self, pool: Pool) -> &SpanPool {
        match pool {
            Pool::Guest => &self.guest,
            Pool::Own => &self.own,
        }
    }

    /// Whether this thread holds the gate of the core this heap is.
    fn holds_gate(&self) -> bool {
        let here = entry::current();
        here.gate && here.heap == ptr::from_ref(self).cast()
    }

    /// Run `pool` over `[span, span + bytes)`, 16-aligned; `give_back`, if
    /// any, returns the span when the core goes. The gate's holder, never the
    /// audio thread (the map may make its nodes). A pool already running goes
    /// first, and what was parked for it with it; its span stays its
    /// lender's. False, with no pool, when the span cannot hold the pool's
    /// headers or the map refuses it: part of a chunk another core's memory
    /// touches. The core's two pools may share a chunk at their ragged edges.
    ///
    /// # Safety
    /// The span must be valid memory that nothing else uses until the pool
    /// stops or the core goes.
    pub unsafe fn start(&self, pool: Pool, span: *mut u8, bytes: usize, give_back: Option<GiveBack>) -> bool {
        debug_assert!(self.holds_gate(), "a pool is started by the gate's holder");
        self.stop(pool);
        if span.is_null() || bytes <= crate::AREA_OVERHEAD || (span as usize) % 16 != 0 {
            return false;
        }
        if !self.claim(pool, span as usize, bytes) {
            return false;
        }
        let p = self.of(pool);
        // SAFETY: the caller's contract; the gate's holder owns the cells.
        unsafe {
            *p.pool.get() = Some(HeapPool::over(span, bytes));
            *p.give_back.get() = give_back;
        }
        p.bytes.store(bytes, Ordering::Release);
        p.base.store(span, Ordering::Release);
        true
    }

    /// Stop `pool`: its span leaves the map and stays its lender's, and what
    /// was parked for it is dropped with it, every block in it the pool's,
    /// which is gone. The gate's holder.
    pub fn stop(&self, pool: Pool) {
        let p = self.of(pool);
        let base = p.base.swap(ptr::null_mut(), Ordering::AcqRel);
        let bytes = p.bytes.swap(0, Ordering::AcqRel);
        if !base.is_null() {
            self.unclaim(pool, base as usize, bytes);
        }
        p.parked.take();
        // SAFETY: the gate's holder owns the cells.
        unsafe {
            *p.pool.get() = None;
            *p.give_back.get() = None;
        }
    }

    /// Whether `pool` is running. Any thread.
    pub fn running(&self, pool: Pool) -> bool {
        !self.of(pool).base.load(Ordering::Acquire).is_null()
    }

    /// `pool`'s span and its length, or null. Any thread.
    pub fn span(&self, pool: Pool) -> (*mut u8, usize) {
        let p = self.of(pool);
        (p.base.load(Ordering::Acquire), p.bytes.load(Ordering::Acquire))
    }

    /// `bytes` from `pool`: null when it is full, not running, asked for 0,
    /// or this thread does not hold the core's gate.
    pub fn take(&self, pool: Pool, bytes: usize) -> *mut u8 {
        if bytes == 0 || !self.holds_gate() {
            return ptr::null_mut();
        }
        let p = self.of(pool);
        // SAFETY: the gate's holder.
        let Some(heap_pool) = (unsafe { &mut *p.pool.get() }) else { return ptr::null_mut() };
        SpanPool::drain(heap_pool, &p.parked);
        heap_pool.alloc(bytes)
    }

    /// Count an allocation `pool` could not meet, which the system served.
    pub fn fell_back(&self, pool: Pool) {
        self.of(pool).fallbacks.fetch_add(1, Ordering::Relaxed);
    }

    /// Give `p` back when it is `pool`'s: freed now by the gate's holder,
    /// parked from any other thread. False, and `p` left alone, when it is
    /// not the pool's.
    ///
    /// # Safety
    /// When `p` is the pool's, it is a block the pool handed out, dead to its
    /// owner.
    pub unsafe fn give(&self, pool: Pool, p: *mut u8) -> bool {
        let sp = self.of(pool);
        if !sp.holds(p) {
            return false;
        }
        if self.holds_gate() {
            // SAFETY: the gate's holder.
            if let Some(heap_pool) = unsafe { &mut *sp.pool.get() } {
                SpanPool::drain(heap_pool, &sp.parked);
                // SAFETY: the caller's contract: a block of this pool.
                unsafe { heap_pool.free(p) };
            }
        } else {
            // SAFETY: the caller's contract: the block is dead and at least
            // 16 bytes, so its first word is ours for the link.
            unsafe { sp.parked.push(p) };
        }
        true
    }

    /// Whether `pool`'s span holds `p`. Any thread.
    pub fn holds(&self, pool: Pool, p: *const u8) -> bool {
        self.of(pool).holds(p)
    }

    /// Which pool's span holds `p`, if either. Any thread.
    pub fn pool_of(&self, p: *const u8) -> Option<Pool> {
        [Pool::Guest, Pool::Own].into_iter().find(|&pool| self.holds(pool, p))
    }

    /// `pool`'s census: the pool's counters, atomics the gate's holder
    /// writes, so any thread reads numbers as fresh as the last block's.
    pub fn census(&self, pool: Pool) -> PoolCensus {
        let p = self.of(pool);
        let fallbacks = p.fallbacks.load(Ordering::Relaxed);
        // SAFETY: the pool is replaced only by `start` and `stop`, the gate's
        // holder's, with no block running; what is read of it is atomic.
        match unsafe { &*p.pool.get() } {
            Some(hp) => PoolCensus { used: hp.used_bytes(), peak: hp.peak_bytes(), double_frees: hp.double_frees(), fallbacks },
            None => PoolCensus { fallbacks, ..PoolCensus::default() },
        }
    }

    /// Walk `pool` and say whether it is whole: a diagnostic that walks
    /// everything. The gate's holder.
    pub fn check(&self, pool: Pool) -> PoolCheck {
        // SAFETY: the gate's holder.
        match unsafe { &*self.of(pool).pool.get() } {
            Some(hp) => hp.check(),
            None => PoolCheck::default(),
        }
    }

    /// Park memory from the host's allocator that a block gave back.
    ///
    /// # Safety
    /// `p` is dead to its owner, 16-aligned and at least 16 bytes.
    pub unsafe fn park_system(&self, p: *mut u8) {
        // SAFETY: the caller's contract.
        unsafe { self.system.push(p) };
    }

    /// Release what [`CoreHeap::park_system`] parked, each through `free`.
    /// Off the audio thread.
    pub fn release_system(&self, mut free: impl FnMut(*mut u8)) {
        let mut node = self.system.take();
        while !node.is_null() {
            // SAFETY: each node was pushed whole, its link written before
            // the release that published it.
            let next = unsafe { (*node).next };
            free(node.cast());
            node = next;
        }
    }

    fn owner(&self) -> NonNull<c_void> {
        NonNull::from(self).cast()
    }

    /// Claim for this heap every chunk `[base, base + bytes)` touches that
    /// the heap's other pool does not hold already. All or none.
    fn claim(&self, pool: Pool, base: usize, bytes: usize) -> bool {
        let Some(chunks) = map::chunks(base, bytes) else { return false };
        let (other, other_bytes) = self.span(pool.other());
        let held = |chunk: usize| map::touches(other as usize, other_bytes, chunk);
        for (i, chunk) in chunks.clone().enumerate() {
            if !held(chunk) && MAP.claim(chunk, self.owner()).is_err() {
                chunks.take(i).filter(|&c| !held(c)).for_each(|c| MAP.release(c, self.owner()));
                return false;
            }
        }
        true
    }

    /// Release the chunks `[base, base + bytes)` touches, but those the
    /// heap's other pool still holds.
    fn unclaim(&self, pool: Pool, base: usize, bytes: usize) {
        let Some(chunks) = map::chunks(base, bytes) else { return };
        let (other, other_bytes) = self.span(pool.other());
        chunks.filter(|&c| !map::touches(other as usize, other_bytes, c)).for_each(|c| MAP.release(c, self.owner()));
    }
}

impl Drop for CoreHeap {
    // The core is going, after its sinks and ports, and nothing else holds
    // its heap. Each pool still running takes back what was parked for it;
    // the core's own must then be empty.
    fn drop(&mut self) {
        let owner = self.owner();
        for (pool, p) in [(Pool::Guest, &mut self.guest), (Pool::Own, &mut self.own)] {
            let base = *p.base.get_mut();
            if base.is_null() {
                continue;
            }
            let bytes = *p.bytes.get_mut();
            let parked = &p.parked;
            let empty = p.pool.get_mut().take().is_none_or(|mut hp| {
                SpanPool::drain(&mut hp, parked);
                hp.used_bytes() == 0
            });
            if pool == Pool::Own && !empty {
                // Memory a free will still reach: kept, not given back under
                // it, and named the tombstone's in the map, so that free is
                // dropped and counted rather than handed to the system.
                MAP.retag(base as usize, bytes, owner, tombstone());
                OUTLIVED.fetch_add(1, Ordering::Relaxed);
                continue;
            }
            MAP.clear(base as usize, bytes, owner);
            if let Some(give_back) = *p.give_back.get_mut() {
                // SAFETY: the span is the lender's again, and nothing of it is
                // in use.
                unsafe { give_back(base.cast()) };
            }
        }
    }
}

/// A pool's census.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolCensus {
    /// Payload bytes in use now.
    pub used: usize,
    /// The most ever in use at once.
    pub peak: usize,
    /// Frees of a block already free, refused: a caller's bug made visible.
    pub double_frees: u32,
    /// Allocations the pool could not meet, which the system served.
    pub fallbacks: u32,
}

/// A lock-free stack of blocks, the link in each block's first word.
struct Parked {
    head: AtomicPtr<Link>,
}

struct Link {
    next: *mut Link,
}

impl Parked {
    const fn new() -> Parked {
        Parked { head: AtomicPtr::new(ptr::null_mut()) }
    }

    /// # Safety
    /// `p` is dead to its owner, 16-aligned and at least 16 bytes.
    unsafe fn push(&self, p: *mut u8) {
        let node = p.cast::<Link>();
        let mut head = self.head.load(Ordering::Relaxed);
        loop {
            // SAFETY: the caller's contract: the first word is ours.
            unsafe { (*node).next = head };
            match self.head.compare_exchange_weak(head, node, Ordering::Release, Ordering::Relaxed) {
                Ok(_) => return,
                Err(h) => head = h,
            }
        }
    }

    /// Everything parked, newest first.
    fn take(&self) -> *mut Link {
        self.head.swap(ptr::null_mut(), Ordering::Acquire)
    }
}

// ── C ABI ───────────────────────────────────────────────────────────────────
//
// A core's heap is reached from C++ by the pointer its core hands out
// (`clockwork_core_heap`). Each call names the heap and the pool; null is
// no heap, and a pool the header does not name is none.

/// `ClockworkHeap`: opaque to C.
pub type ClockworkHeap = CoreHeap;

/// `ClockworkPool`. A newtype, as the client's enums are.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClockworkPool(pub c_int);

impl ClockworkPool {
    pub const GUEST: ClockworkPool = ClockworkPool(0);
    pub const OWN: ClockworkPool = ClockworkPool(1);

    fn pool(self) -> Option<Pool> {
        match self {
            ClockworkPool::GUEST => Some(Pool::Guest),
            ClockworkPool::OWN => Some(Pool::Own),
            _ => None,
        }
    }
}

/// The heap and pool a C call names, if it names both.
///
/// # Safety
/// `heap` is null or a live core's heap.
unsafe fn named<'a>(heap: *const ClockworkHeap, pool: ClockworkPool) -> Option<(&'a CoreHeap, Pool)> {
    // SAFETY: the caller's contract.
    Some((unsafe { heap.as_ref() }?, pool.pool()?))
}

/// # Safety
/// `heap` is null or a live core's heap, and the caller holds the core's
/// gate; the span is [`CoreHeap::start`]'s.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_start(
    heap: *const ClockworkHeap,
    pool: ClockworkPool,
    span: *mut c_void,
    bytes: usize,
    give_back: Option<GiveBack>,
) -> bool {
    no_unwind(false, || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.is_some_and(|(h, p)| unsafe { h.start(p, span.cast(), bytes, give_back) })
    })
}

/// # Safety
/// `heap` is null or a live core's heap, and the caller holds its gate.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_stop(heap: *const ClockworkHeap, pool: ClockworkPool) {
    no_unwind((), || {
        // SAFETY: the caller's contract.
        if let Some((h, p)) = unsafe { named(heap, pool) } {
            h.stop(p);
        }
    })
}

/// # Safety
/// `heap` is null or a live core's heap.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_running(heap: *const ClockworkHeap, pool: ClockworkPool) -> bool {
    no_unwind(false, || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.is_some_and(|(h, p)| h.running(p))
    })
}

/// The pool's span, or null; its length through `bytes` when that is not
/// null.
///
/// # Safety
/// `heap` is null or a live core's heap; `bytes` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_span(heap: *const ClockworkHeap, pool: ClockworkPool, bytes: *mut usize) -> *mut c_void {
    no_unwind(core::ptr::null_mut(), || {
        // SAFETY: the caller's contract.
        let (span, len) = unsafe { named(heap, pool) }.map_or((ptr::null_mut(), 0), |(h, p)| h.span(p));
        if !bytes.is_null() {
            // SAFETY: the caller's contract.
            unsafe { *bytes = len };
        }
        span.cast()
    })
}

/// # Safety
/// `heap` is null or a live core's heap.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_take(heap: *const ClockworkHeap, pool: ClockworkPool, bytes: usize) -> *mut c_void {
    no_unwind(core::ptr::null_mut(), || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.map_or(ptr::null_mut(), |(h, p)| h.take(p, bytes).cast())
    })
}

/// # Safety
/// `heap` is null or a live core's heap; `p` as [`CoreHeap::give`].
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_give(heap: *const ClockworkHeap, pool: ClockworkPool, p: *mut c_void) -> bool {
    no_unwind(false, || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.is_some_and(|(h, which)| unsafe { h.give(which, p.cast()) })
    })
}

/// # Safety
/// `heap` is null or a live core's heap.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_holds(heap: *const ClockworkHeap, pool: ClockworkPool, p: *const c_void) -> bool {
    no_unwind(false, || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.is_some_and(|(h, which)| h.holds(which, p.cast()))
    })
}

/// # Safety
/// `heap` is null or a live core's heap.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_census(heap: *const ClockworkHeap, pool: ClockworkPool) -> PoolCensus {
    no_unwind(PoolCensus::default(), || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.map_or_else(PoolCensus::default, |(h, p)| h.census(p))
    })
}

/// # Safety
/// `heap` is null or a live core's heap, and the caller holds its gate.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_check(heap: *const ClockworkHeap, pool: ClockworkPool) -> PoolCheck {
    no_unwind(PoolCheck::default(), || {
        // SAFETY: the caller's contract.
        unsafe { named(heap, pool) }.map_or_else(PoolCheck::default, |(h, p)| h.check(p))
    })
}

/// # Safety
/// `heap` is null or a live core's heap; `p` as [`CoreHeap::park_system`].
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_park_system(heap: *const ClockworkHeap, p: *mut c_void) {
    no_unwind((), || {
        // SAFETY: the caller's contract.
        if let Some(h) = unsafe { heap.as_ref() } {
            // SAFETY: the caller's contract for `p`.
            unsafe { h.park_system(p.cast()) };
        }
    })
}

/// Release what was parked, each through `free`; a null `free` forgets
/// them, for memory that is going whole.
///
/// # Safety
/// `heap` is null or a live core's heap; `free` takes back what the host's
/// allocator gave.
#[no_mangle]
pub unsafe extern "C" fn clockwork_heap_release_system(heap: *const ClockworkHeap, free: Option<unsafe extern "C" fn(*mut c_void)>) {
    no_unwind((), || {
        // SAFETY: the caller's contract.
        let Some(h) = (unsafe { heap.as_ref() }) else { return };
        match free {
            // SAFETY: the caller's contract: `free` takes each back.
            Some(free) => h.release_system(|p| unsafe { free(p.cast()) }),
            None => h.release_system(|_| {}),
        }
    })
}

/// Cores that went with clockwork memory still in their own pool.
#[no_mangle]
pub extern "C" fn clockwork_heap_outlived() -> u32 {
    no_unwind(0, || {
        outlived()
    })
}

/// Blocks of a core's own pool freed after the core went: dropped, counted.
#[no_mangle]
pub extern "C" fn clockwork_heap_outlived_frees() -> u32 {
    no_unwind(0, || {
        outlived_frees()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::entry::{Entry, Kind};
    use crate::map::CHUNK_BYTES;
    use std::sync::atomic::AtomicUsize as Count;

    // A span from the system, on a chunk: what a core's memory gives a pool.
    struct Span {
        raw: *mut u8,
        layout: std::alloc::Layout,
    }
    impl Span {
        fn new(bytes: usize) -> Span {
            let layout = std::alloc::Layout::from_size_align(bytes, CHUNK_BYTES).unwrap();
            // SAFETY: a non-zero size.
            let raw = unsafe { std::alloc::alloc(layout) };
            assert!(!raw.is_null());
            Span { raw, layout }
        }
    }
    impl Drop for Span {
        fn drop(&mut self) {
            // SAFETY: from `alloc` with this layout.
            unsafe { std::alloc::dealloc(self.raw, self.layout) };
        }
    }

    // This thread holding `heap`'s gate until the scope ends.
    struct Held(Entry);
    impl Held {
        fn gate(heap: &CoreHeap) -> Held {
            Held(entry::enter(Entry { core: ptr::null_mut(), heap: ptr::from_ref(heap).cast(), kind: Kind::Control, gate: true }))
        }
    }
    impl Drop for Held {
        fn drop(&mut self) {
            entry::leave(self.0);
        }
    }

    #[test]
    fn the_guests_pool_runs_over_its_span_and_the_map_names_its_heap() {
        let heap = Box::new(CoreHeap::new());
        let span = Span::new(2 * CHUNK_BYTES);
        let _held = Held::gate(&heap);
        // SAFETY: the span is the heap's until stop.
        assert!(unsafe { heap.start(Pool::Guest, span.raw, 2 * CHUNK_BYTES, None) });
        let p = heap.take(Pool::Guest, 100);
        assert_eq!(heap.pool_of(p), Some(Pool::Guest));
        assert_eq!(MAP.owner_of(p as usize), ptr::from_ref::<CoreHeap>(&heap).cast_mut().cast());
        assert!(heap.census(Pool::Guest).used >= 100);
        // SAFETY: from take, given back once.
        assert!(unsafe { heap.give(Pool::Guest, p) });
        assert_eq!(heap.census(Pool::Guest).used, 0);
        assert_eq!(heap.check(Pool::Guest).code, 0);
        heap.stop(Pool::Guest);
        assert!(MAP.owner_of(span.raw as usize).is_null());
        assert!(heap.take(Pool::Guest, 100).is_null(), "a stopped pool gives nothing");
    }

    // A pool never grows (clockwork_heap.h): it fills its span, refuses
    // what the span cannot hold, takes nothing from the system for it, and
    // takes again what is given back. What a guest declared is what it has.
    #[test]
    fn a_pool_fills_its_span_then_refuses_and_never_grows() {
        const SPAN: usize = 16 * CHUNK_BYTES;
        let heap = Box::new(CoreHeap::new());
        let span = Span::new(SPAN);
        let _held = Held::gate(&heap);
        // SAFETY: the span is the heap's until stop.
        assert!(unsafe { heap.start(Pool::Guest, span.raw, SPAN, None) });
        assert!(heap.take(Pool::Guest, 2 * SPAN).is_null(), "more than the span");
        let mut taken = Vec::new();
        loop {
            let p = heap.take(Pool::Guest, CHUNK_BYTES);
            if p.is_null() {
                break;
            }
            taken.push(p);
            assert!(taken.len() < 16, "the span is finite");
        }
        assert!(!taken.is_empty());
        let within = span.raw as usize..span.raw as usize + SPAN;
        assert!(taken.iter().all(|&p| within.contains(&(p as usize)) && heap.holds(Pool::Guest, p)),
                "every block from the span, none from the system");
        for p in taken {
            // SAFETY: from take, given back once.
            assert!(unsafe { heap.give(Pool::Guest, p) });
        }
        assert_eq!(heap.census(Pool::Guest).used, 0);
        let again = heap.take(Pool::Guest, CHUNK_BYTES);
        assert!(!again.is_null(), "taken again once given back");
        // SAFETY: as above.
        assert!(unsafe { heap.give(Pool::Guest, again) });
        heap.stop(Pool::Guest);
    }

    // A span off the chunks: the map names the ragged chunks, and the heap
    // owns only its exact span within them.
    #[test]
    fn a_span_off_the_chunks_holds_only_itself() {
        let heap = Box::new(CoreHeap::new());
        let span = Span::new(2 * CHUNK_BYTES);
        let _held = Held::gate(&heap);
        // SAFETY: as above, the span within the allocation.
        let inner = unsafe { span.raw.add(4096) };
        // SAFETY: the span is the heap's until stop.
        assert!(unsafe { heap.start(Pool::Guest, inner, CHUNK_BYTES, None) });
        assert_eq!(MAP.owner_of(span.raw as usize), ptr::from_ref::<CoreHeap>(&heap).cast_mut().cast());
        assert_eq!(heap.pool_of(span.raw), None, "the ragged edge is not the heap's");
        assert_eq!(heap.pool_of(inner), Some(Pool::Guest));
        // SAFETY: not the pool's: left alone.
        assert!(!unsafe { heap.give(Pool::Guest, span.raw) });
        heap.stop(Pool::Guest);
        // SAFETY: as above.
        assert!(!unsafe { heap.start(Pool::Guest, span.raw.add(8), CHUNK_BYTES, None) }, "16-aligned only");
        assert!(MAP.owner_of(span.raw as usize).is_null());
    }

    #[test]
    fn the_two_pools_are_apart_and_each_answers_for_its_own_span() {
        let heap = Box::new(CoreHeap::new());
        let (guest, own) = (Span::new(CHUNK_BYTES), Span::new(CHUNK_BYTES));
        let _held = Held::gate(&heap);
        // SAFETY: the spans are the heap's until stop.
        unsafe {
            assert!(heap.start(Pool::Guest, guest.raw, CHUNK_BYTES, None));
            assert!(heap.start(Pool::Own, own.raw, CHUNK_BYTES, None));
        }
        let (g, o) = (heap.take(Pool::Guest, 64), heap.take(Pool::Own, 64));
        assert_eq!((heap.pool_of(g), heap.pool_of(o)), (Some(Pool::Guest), Some(Pool::Own)));
        // SAFETY: not the guest pool's: left alone.
        assert!(!unsafe { heap.give(Pool::Guest, o) }, "one pool took the other's block");
        // SAFETY: from take, given back once each.
        unsafe {
            assert!(heap.give(Pool::Own, o));
            assert!(heap.give(Pool::Guest, g));
        }
        heap.stop(Pool::Guest);
        heap.stop(Pool::Own);
    }

    // The core's two pools side by side in one chunk: each starts, and one
    // stopping leaves the other found.
    #[test]
    fn two_pools_of_one_core_may_share_a_chunk() {
        let heap = Box::new(CoreHeap::new());
        let span = Span::new(2 * CHUNK_BYTES);
        let _held = Held::gate(&heap);
        let half = CHUNK_BYTES / 2;
        // SAFETY: the spans are the heap's until stop, apart within the allocation.
        unsafe {
            assert!(heap.start(Pool::Guest, span.raw, half, None));
            assert!(heap.start(Pool::Own, span.raw.add(half), CHUNK_BYTES, None), "the shared chunk refused the core's own");
        }
        heap.stop(Pool::Guest);
        let o = heap.take(Pool::Own, 64);
        assert_eq!(heap.pool_of(o), Some(Pool::Own));
        assert_eq!(MAP.owner_of(span.raw as usize), ptr::from_ref::<CoreHeap>(&heap).cast_mut().cast(), "the guest's stop forgot the own pool's chunk");
        // SAFETY: from take, given back once.
        assert!(unsafe { heap.give(Pool::Own, o) });
        heap.stop(Pool::Own);
        assert!(MAP.owner_of(span.raw as usize).is_null());
        assert!(MAP.owner_of(span.raw as usize + CHUNK_BYTES).is_null());
    }

    #[test]
    fn a_thread_without_the_gate_takes_nothing_and_its_free_waits_for_the_holder() {
        let heap = Box::new(CoreHeap::new());
        let span = Span::new(CHUNK_BYTES);
        let p = {
            let _held = Held::gate(&heap);
            // SAFETY: as above.
            assert!(unsafe { heap.start(Pool::Own, span.raw, CHUNK_BYTES, None) });
            heap.take(Pool::Own, 256)
        };
        let addr = p.expose_provenance();
        std::thread::scope(|s| {
            s.spawn(|| {
                assert!(heap.take(Pool::Own, 64).is_null(), "a thread without the gate took from the pool");
                // SAFETY: from take, given back once.
                assert!(unsafe { heap.give(Pool::Own, ptr::with_exposed_provenance_mut(addr)) });
            });
        });
        let _held = Held::gate(&heap);
        assert!(heap.census(Pool::Own).used >= 256, "a free without the gate reached the pool");
        let q = heap.take(Pool::Own, 256);
        assert_eq!(q, p, "the parked block comes back to the gate's holder");
        // SAFETY: from take, given back once.
        unsafe { heap.give(Pool::Own, q) };
        heap.stop(Pool::Own);
    }

    // Suspect 3 (docs/ENGINES.md): a block parked for one instance's
    // pool must never be freed into the next one's, over the same span.
    #[test]
    fn a_free_parked_before_a_restart_never_reaches_the_new_pool() {
        let heap = Box::new(CoreHeap::new());
        let span = Span::new(CHUNK_BYTES);
        let _held = Held::gate(&heap);
        // SAFETY: as above.
        assert!(unsafe { heap.start(Pool::Guest, span.raw, CHUNK_BYTES, None) });
        let old = heap.take(Pool::Guest, 256).expose_provenance();
        std::thread::scope(|s| {
            // SAFETY: from take, given back once.
            s.spawn(|| assert!(unsafe { heap.give(Pool::Guest, ptr::with_exposed_provenance_mut(old)) }));
        });
        // SAFETY: as above.
        assert!(unsafe { heap.start(Pool::Guest, span.raw, CHUNK_BYTES, None) });
        let a = heap.take(Pool::Guest, 256);
        assert_eq!(a as usize, old, "the new pool hands out the same place first");
        let b = heap.take(Pool::Guest, 256);
        assert_ne!(a, b, "the old free reached the new pool and freed a live block");
        assert_eq!(heap.census(Pool::Guest).double_frees, 0);
        heap.stop(Pool::Guest);
    }

    static GIVEN_BACK: Count = Count::new(0);
    unsafe extern "C" fn count_give_back(_span: *mut c_void) {
        GIVEN_BACK.fetch_add(1, Ordering::Relaxed);
    }

    // The core's own pool goes when the core does: empty, its span goes back
    // to its lender; with something still in it — a table a drain thread was
    // mid-way through, say — the span is kept, named the tombstone's, and
    // the core counted; and the block's later free through the allocator is
    // dropped and counted, never handed to the system allocator as if it
    // were the system's. All in one case, as all read process-wide counts.
    #[test]
    #[cfg_attr(miri, ignore = "keeps a span for good, as the product does, which Miri reports as a leak")]
    fn a_cores_own_pool_gives_its_span_back_empty_and_is_counted_when_not() {
        use std::alloc::{GlobalAlloc, Layout};
        let span = Span::new(CHUNK_BYTES);
        let (given, outlived_before, frees) = (GIVEN_BACK.load(Ordering::Relaxed), outlived(), outlived_frees());
        {
            let heap = Box::new(CoreHeap::new());
            let p = {
                let _held = Held::gate(&heap);
                // SAFETY: the span is the heap's until it goes.
                assert!(unsafe { heap.start(Pool::Own, span.raw, CHUNK_BYTES, Some(count_give_back)) });
                heap.take(Pool::Own, 128)
            };
            // Given back without the gate, as the core's tables go: parked,
            // and taken back as the heap goes.
            // SAFETY: from take, given back once.
            assert!(unsafe { heap.give(Pool::Own, p) });
        }
        assert_eq!(GIVEN_BACK.load(Ordering::Relaxed), given + 1, "the empty pool's span went back");
        assert_eq!(outlived(), outlived_before);
        assert!(MAP.owner_of(span.raw as usize).is_null());

        let held = {
            let heap = Box::new(CoreHeap::new());
            let _held = Held::gate(&heap);
            // SAFETY: as above.
            assert!(unsafe { heap.start(Pool::Own, span.raw, CHUNK_BYTES, Some(count_give_back)) });
            heap.take(Pool::Own, 128)
        };
        assert!(!held.is_null());
        assert_eq!(GIVEN_BACK.load(Ordering::Relaxed), given + 1, "a span still in use went back");
        assert_eq!(outlived(), outlived_before + 1);
        // Kept, and the tombstone's in the map: a free that reaches it is
        // dropped there and counted.
        assert_eq!(MAP.owner_of(span.raw as usize), tombstone().as_ptr());
        // SAFETY: a block the pool handed out, freed once, as the allocator
        // would free a Vec's buffer.
        unsafe { crate::router::RtRouter.dealloc(held, Layout::from_size_align(128, 8).unwrap()) };
        assert_eq!(outlived_frees(), frees + 1);
        // Kept here too, as the product keeps it: the tombstone names its
        // chunks for good, so the system must never hand them to another span.
        std::mem::forget(span);
    }

    // What the host's allocator hands out: 16-aligned.
    #[repr(align(16))]
    struct HostBlock {
        _bytes: [u8; 32],
    }

    #[test]
    fn a_hosts_memory_freed_during_a_block_waits_to_be_released() {
        let heap = CoreHeap::new();
        let block = Box::into_raw(Box::new(HostBlock { _bytes: [0; 32] })).cast::<u8>();
        // SAFETY: dead to its owner, 16-aligned, 32 bytes.
        unsafe { heap.park_system(block) };
        let mut released = Vec::new();
        heap.release_system(|p| released.push(p));
        assert_eq!(released, vec![block]);
        // SAFETY: from Box::into_raw above.
        drop(unsafe { Box::from_raw(block.cast::<HostBlock>()) });
        heap.release_system(|_| panic!("released twice"));
    }
}
