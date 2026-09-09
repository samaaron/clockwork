// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The timed-event store.
//!
//! Opaque payload bytes keyed by an int64 timetag, released in time order,
//! each carrying a caller-defined metadata record and a tag used for selective
//! cancellation.
//!
//! # The three pools
//!
//! * **slots** — a fixed array of records, threaded into a free list. One slot
//!   per live event.
//! * **data** — one byte pool, bump-allocated. A freed slot leaves a hole;
//!   holes are reclaimed by [`Store::compact`], which slides the live chunks
//!   down in place. When the last slot goes the head simply resets to 0.
//! * **meta** — `slot_count * meta_size` bytes, aligned to `meta_align`,
//!   addressed by slot index. This crate NEVER reads or writes them: the
//!   caller is handed a pointer to its slot's bytes by [`Store::add`] and
//!   constructs whatever it likes there. That is what keeps the store generic
//!   over a C++ type it cannot name.
//!
//! Ordering is a binary min-heap of `(when, stability)`. `stability` is a
//! monotonic counter stamped at `add` and never reset, so no two live entries
//! can compare equal: the pop order is a total order fixed entirely by the
//! call sequence, not by the heap's internal arrangement. That is why this
//! implementation and the C++ one it replaces pop identically without either
//! having to reproduce the other's sift.
//!
//! # Real-time safety
//!
//! Every allocation is in [`Store::new`]. `add`, `pop_due`, `release`,
//! `flush`, `clear` and `compact` allocate nothing, lock nothing and cannot
//! panic: every index is derived from the free list or the heap, both of which
//! only ever hold in-range values, and the two size computations that could
//! overflow a `u32` are done in `u64`.

use core::mem::ManuallyDrop;
use core::sync::atomic::{AtomicBool, Ordering};
use std::alloc::{alloc_zeroed, dealloc, Layout};

/// The largest slot pool this store will build. The C++ header it implements
/// keeps slot indices in an `int16_t`, and its `static_assert` says so; the
/// limit is kept here because the ABI takes the count at runtime and something
/// has to refuse an impossible one.
pub const MAX_SLOT_COUNT: u32 = 32767;

/// The widest alignment any member needs; `bytes_needed` adds this much slack
/// so a caller's buffer does not have to be over-aligned.
const MAX_MEMBER_ALIGN: usize = 16;

#[inline]
fn align_up(v: usize, align: usize) -> usize {
    (v + align - 1) & !(align - 1)
}

#[derive(Clone, Copy)]
struct Slot {
    when: i64,
    stability: i64,
    tag: u32,
    offset: u32,
    size: u32,
    in_use: bool,
    /// Still in the queue, i.e. not yet handed out by `pop_due`.
    ///
    /// `in_use` alone conflated two states — "this slot is occupied" and "this
    /// event has not fired yet" — and both flush and rebuild_heap read it as
    /// the second. Between pop_due and release a slot is occupied but NOT
    /// pending: the caller is holding that event and reading its payload.
    pending: bool,
    next_free: i32,
}

/// One heap entry. Ordered by `when`, then by `stability` so equal timetags
/// fire in the order they were added (FIFO).
#[derive(Clone, Copy)]
struct Entry {
    when: i64,
    stability: i64,
    slot: i32,
}

impl Entry {
    #[inline]
    fn earlier_than(&self, o: &Entry) -> bool {
        if self.when != o.when {
            self.when < o.when
        } else {
            self.stability < o.stability
        }
    }
}

/// A live data chunk, for `compact`'s working set. Held as a member so the
/// sort never touches the realtime stack.
#[derive(Clone, Copy)]
struct Live {
    off: u32,
    size: u32,
    slot: i32,
}

/// A due event, handed to the caller. `meta` and `data` borrow the store's
/// pools; the C ABI turns them into pointers that stay valid until
/// [`Store::release`] is called for `slot`, and a Rust caller copies what it
/// keeps before releasing.
pub struct Event<'a> {
    pub when: i64,
    pub tag: u32,
    pub meta: &'a [u8],
    pub data: &'a [u8],
    pub slot: i32,
}

/// The caller's metadata, one fixed-size record per slot, opaque to us.
struct MetaArena {
    ptr: *mut u8,
    layout: Layout,
    stride: usize,
    /// False when the memory came from the caller and is not ours to free.
    owned: bool,
}

impl MetaArena {
    fn new(slot_count: usize, meta_size: usize, meta_align: usize) -> Option<Self> {
        if meta_size == 0 || !meta_align.is_power_of_two() || !meta_size.is_multiple_of(meta_align) {
            return None;
        }
        let bytes = meta_size.checked_mul(slot_count)?;
        let layout = Layout::from_size_align(bytes, meta_align).ok()?;
        // Zeroed so a caller that reads a slot it never wrote sees zeros
        // rather than whatever the allocator had, matching a C++ member array
        // in BSS.
        // SAFETY: never zero-sized: `meta_size` is non-zero and `Store::new`,
        // the one caller, refuses a zero slot count.
        let ptr = unsafe { alloc_zeroed(layout) };
        if ptr.is_null() {
            return None;
        }
        Some(MetaArena { ptr, layout, stride: meta_size, owned: true })
    }

    /// Record `i`, as bytes.
    #[inline]
    fn slot(&self, i: usize) -> &[u8] {
        // SAFETY: `i < slot_count` at every call site; the arena is
        // `slot_count * stride` bytes, zeroed at creation.
        unsafe { core::slice::from_raw_parts(self.ptr.add(i * self.stride), self.stride) }
    }

    /// Record `i`, writable.
    #[inline]
    fn slot_mut(&mut self, i: usize) -> &mut [u8] {
        // SAFETY: as `slot`, and `&mut self` makes the access exclusive.
        unsafe { core::slice::from_raw_parts_mut(self.ptr.add(i * self.stride), self.stride) }
    }
}

impl Drop for MetaArena {
    fn drop(&mut self) {
        if self.owned {
            // SAFETY: `owned` means `new` allocated it with this layout, and
            // this is its one free.
            unsafe { dealloc(self.ptr, self.layout) }
        }
    }
}

pub struct Store {
    /*
     * ManuallyDrop, because the backing is not always ours.
     *
     * `new` allocates and owns it; `new_in` carves it out of memory the
     * caller supplies — a static array, in clockwork's case — and must not
     * free it. Deref still gives `[T]`, so every index, len() and
     * copy_within() below is unchanged.
     */
    slots: ManuallyDrop<Box<[Slot]>>,
    queue: ManuallyDrop<Box<[Entry]>>,
    scratch: ManuallyDrop<Box<[Live]>>,
    data: ManuallyDrop<Box<[u8]>>,
    meta: MetaArena,
    data_head: u32,
    queue_size: usize,
    live: i32,
    stability: i64,
    free_head: i32,
    clear_pending: AtomicBool,
    /// True when `new` allocated the four buffers above and Drop must free them.
    owned: bool,
}

impl Drop for Store {
    fn drop(&mut self) {
        if self.owned {
            // SAFETY: `owned` means the four boxes came from `new`'s
            // `into_boxed_slice`, dropped exactly once, here; `new_in`'s
            // borrowed backing never reaches this branch.
            unsafe {
                ManuallyDrop::drop(&mut self.slots);
                ManuallyDrop::drop(&mut self.queue);
                ManuallyDrop::drop(&mut self.scratch);
                ManuallyDrop::drop(&mut self.data);
            }
        }
    }
}

impl Store {
    /// Build a store. The only allocating call in this file.
    ///
    /// `meta_size` / `meta_align` describe the caller's per-event record;
    /// nothing here interprets it. Returns `None` for a count outside
    /// [`MAX_SLOT_COUNT`], an empty data pool, a meta description that is not
    /// a valid C layout, or an allocation failure.
    pub fn new(
        slot_count: u32,
        data_pool_size: u32,
        meta_size: u32,
        meta_align: u32,
    ) -> Option<Store> {
        if slot_count == 0 || slot_count > MAX_SLOT_COUNT || data_pool_size == 0 {
            return None;
        }
        let n = slot_count as usize;
        let meta = MetaArena::new(n, meta_size as usize, meta_align as usize)?;
        let mut s = Store {
            slots: ManuallyDrop::new(vec![
                Slot {
                    when: 0,
                    stability: 0,
                    tag: 0,
                    offset: 0,
                    size: 0,
                    in_use: false,
                    pending: false,
                    next_free: -1,
                };
                n
            ]
            .into_boxed_slice()),
            queue: ManuallyDrop::new(
                vec![Entry { when: 0, stability: 0, slot: -1 }; n].into_boxed_slice()),
            scratch: ManuallyDrop::new(
                vec![Live { off: 0, size: 0, slot: -1 }; n].into_boxed_slice()),
            data: ManuallyDrop::new(vec![0u8; data_pool_size as usize].into_boxed_slice()),
            meta,
            data_head: 0,
            queue_size: 0,
            live: 0,
            stability: 0,
            free_head: -1,
            clear_pending: AtomicBool::new(false),
            owned: true,
        };
        s.reset();
        Some(s)
    }

    /*
     * How many bytes `new_in` needs for this shape.
     *
     * Callers size a static array from this, so it must be computable without
     * allocating and without a Store existing yet.
     */
    pub fn bytes_needed(
        slot_count: u32,
        data_pool_size: u32,
        meta_size: u32,
        meta_align: u32,
    ) -> Option<usize> {
        if slot_count == 0 || slot_count > MAX_SLOT_COUNT || data_pool_size == 0 {
            return None;
        }
        if meta_size == 0 || !(meta_align as usize).is_power_of_two()
            || !meta_size.is_multiple_of(meta_align)
        {
            return None;
        }
        let n = slot_count as usize;
        let mut total: usize = 0;
        for (size, align) in [
            (core::mem::size_of::<Slot>().checked_mul(n)?,  core::mem::align_of::<Slot>()),
            (core::mem::size_of::<Entry>().checked_mul(n)?, core::mem::align_of::<Entry>()),
            (core::mem::size_of::<Live>().checked_mul(n)?,  core::mem::align_of::<Live>()),
            (data_pool_size as usize, 1usize),
            ((meta_size as usize).checked_mul(n)?, meta_align as usize),
        ] {
            total = align_up(total, align).checked_add(size)?;
        }
        // One extra alignment slack so a caller's buffer need not be
        // over-aligned for the widest member.
        total.checked_add(MAX_MEMBER_ALIGN)
    }

    /*
     * Build a store inside memory the caller owns. Allocates NOTHING.
     *
     * This exists because the owning constructor allocates, and clockwork
     * builds its scheduler from a file-scope static whose constructor runs
     * during static initialisation — before the heap is arranged. That
     * allocation failed, clockwork_sched_new returned null, and a null handle
     * refuses every add exactly as a full queue does. The scheduler was dead
     * for the life of the web build and the only symptom was a dropped count
     * that read as backpressure.
     *
     * Given a static array there is nothing to fail: construction cannot
     * allocate, so it cannot lose a race with heap setup, and it does not
     * compete for a malloc budget that something else was quietly relying on.
     */
    /// # Safety
    /// `mem` must point at `len` writable bytes that outlive the Store.
    pub unsafe fn new_in(
        mem: *mut u8,
        len: usize,
        slot_count: u32,
        data_pool_size: u32,
        meta_size: u32,
        meta_align: u32,
    ) -> Option<Store> {
        let needed = Store::bytes_needed(slot_count, data_pool_size, meta_size, meta_align)?;
        if mem.is_null() || len < needed {
            return None;
        }
        let n = slot_count as usize;
        let base = mem as usize;
        let mut off = 0usize;

        let mut carve = |size: usize, align: usize| -> *mut u8 {
            off = align_up(base + off, align) - base;
            // SAFETY: the regions carved here are exactly those
            // `bytes_needed` summed, so `off + size <= needed <= len` and
            // the pointer stays inside the caller's buffer.
            let p = unsafe { mem.add(off) };
            off += size;
            p
        };

        let slots_p = carve(core::mem::size_of::<Slot>() * n, core::mem::align_of::<Slot>())
            .cast::<Slot>();
        let queue_p = carve(core::mem::size_of::<Entry>() * n, core::mem::align_of::<Entry>())
            .cast::<Entry>();
        let scratch_p = carve(core::mem::size_of::<Live>() * n, core::mem::align_of::<Live>())
            .cast::<Live>();
        let data_p = carve(data_pool_size as usize, 1);
        let meta_p = carve(meta_size as usize * n, meta_align as usize);

        // The carved regions are uninitialised; reset() below writes every
        // slot and entry, and the data pool is only ever read back through
        // offsets that a write established first.
        // SAFETY: `len` writable bytes, per the contract.
        unsafe { core::ptr::write_bytes(mem, 0, needed.min(len)) };

        // SAFETY: each region was carved at its type's alignment with room
        // for `n` elements (or the pool's bytes) and zeroed above, which is
        // a valid value for every field type here. The boxes sit in
        // `ManuallyDrop` and `owned` is false, so they are never freed: the
        // memory is the caller's, not the allocator's.
        let (slots, queue, scratch, data) = unsafe {
            (
                Box::from_raw(core::ptr::slice_from_raw_parts_mut(slots_p, n)),
                Box::from_raw(core::ptr::slice_from_raw_parts_mut(queue_p, n)),
                Box::from_raw(core::ptr::slice_from_raw_parts_mut(scratch_p, n)),
                Box::from_raw(core::ptr::slice_from_raw_parts_mut(data_p, data_pool_size as usize)),
            )
        };
        let mut s = Store {
            slots: ManuallyDrop::new(slots),
            queue: ManuallyDrop::new(queue),
            scratch: ManuallyDrop::new(scratch),
            data: ManuallyDrop::new(data),
            meta: MetaArena {
                ptr: meta_p,
                layout: Layout::from_size_align(meta_size as usize * n, meta_align as usize).ok()?,
                stride: meta_size as usize,
                owned: false,
            },
            data_head: 0,
            queue_size: 0,
            live: 0,
            stability: 0,
            free_head: -1,
            clear_pending: AtomicBool::new(false),
            owned: false,
        };
        s.reset();
        Some(s)
    }

    // ── The caller's surface ────────────────────────────────────────────────

    /// Copy `data` in, to fire at `when` under `tag`.
    ///
    /// Returns the caller's metadata record for this event — the caller
    /// writes its record there — or `None` if the slot pool or the data pool
    /// is full, with no state changed either way.
    pub fn add(&mut self, when: i64, tag: u32, data: &[u8]) -> Option<&mut [u8]> {
        if self.queue_size >= self.slots.len() {
            return None;
        }
        let size = u32::try_from(data.len()).ok()?;
        let pool = self.data.len() as u64;
        if size as u64 > pool {
            // Never fits, whatever we free. Checked before the align below so
            // that cannot overflow either.
            return None;
        }
        let aligned = (size as u64 + 3) & !3;
        if self.data_head as u64 + aligned > pool {
            self.compact();
            if self.data_head as u64 + aligned > pool {
                return None;
            }
        }

        let slot = self.alloc_slot()?;

        let offset = self.data_head;
        self.data[offset as usize..offset as usize + data.len()].copy_from_slice(data);
        self.data_head += aligned as u32;

        let stability = self.stability;
        self.stability += 1;
        let s = &mut self.slots[slot];
        s.when = when;
        s.tag = tag;
        s.offset = offset;
        s.size = size;
        s.stability = stability;
        s.in_use = true;
        s.pending = true;

        self.heap_push(Entry { when, stability, slot: slot as i32 });
        Some(self.meta.slot_mut(slot))
    }

    /// Timetag of the earliest live event, or `i64::MAX` if none.
    #[inline]
    pub fn next_time(&self) -> i64 {
        if self.queue_size > 0 {
            self.queue[0].when
        } else {
            i64::MAX
        }
    }

    /// Take the earliest event if it is due at or before `now`. The returned
    /// slices borrow the pools; release the slot once they are done with.
    pub fn pop_due(&mut self, now: i64) -> Option<Event<'_>> {
        if self.queue_size == 0 || self.queue[0].when > now {
            return None;
        }
        let slot = self.queue[0].slot as usize;
        self.heap_pop();
        // Out of the queue and into the caller's hands: occupied, not pending.
        self.slots[slot].pending = false;
        let s = self.slots[slot];
        Some(Event {
            when: s.when,
            tag: s.tag,
            meta: self.meta.slot(slot),
            // `offset + size <= data.len()` holds for every live slot.
            data: &self.data[s.offset as usize..s.offset as usize + s.size as usize],
            slot: slot as i32,
        })
    }

    /// Return a popped event's slot to the pool. Out-of-range and
    /// already-free slots are no-ops, as the C++ header's `release` is.
    pub fn release(&mut self, slot: i32) {
        if slot < 0 || slot as usize >= self.slots.len() {
            return;
        }
        self.free_slot(slot as usize);
    }

    /// Cancel every live event whose tag matches. Tag 0 is the wildcard and
    /// clears the store. The heap is rebuilt from the survivors, so it never
    /// carries dead entries — a tombstone design would never reclaim a flushed
    /// event buried under a live one.
    pub fn flush(&mut self, tag: u32) {
        if tag == 0 {
            self.reset();
            return;
        }
        let mut freed_any = false;
        for i in 0..self.slots.len() {
            if self.slots[i].in_use && self.slots[i].pending
                && self.slots[i].tag == tag {
                self.free_slot(i);
                freed_any = true;
            }
        }
        if freed_any {
            self.rebuild_heap();
        }
    }

    #[inline]
    pub fn clear(&mut self) {
        self.reset();
    }

    #[inline]
    pub fn size(&self) -> i32 {
        self.live
    }

    #[inline]
    pub fn full(&self) -> bool {
        self.queue_size >= self.slots.len()
    }

    #[inline]
    pub fn data_used(&self) -> u32 {
        self.data_head
    }

    #[inline]
    pub fn data_capacity(&self) -> u32 {
        self.data.len() as u32
    }

    /// Cross-thread clear handshake. A control thread requests; the thread
    /// that owns the store drains at a safe point. Release pairs with the
    /// acquire in [`Store::drain_pending_clear`].
    #[inline]
    pub fn request_clear(&self) {
        self.clear_pending.store(true, Ordering::Release);
    }

    /// Perform a pending clear, if one was requested. The flag is consumed.
    pub fn drain_pending_clear(&mut self) -> bool {
        if self.clear_pending.swap(false, Ordering::Acquire) {
            self.reset();
            true
        } else {
            false
        }
    }

    // ── Slots ───────────────────────────────────────────────────────────────

    fn reset(&mut self) {
        self.data_head = 0;
        self.queue_size = 0;
        self.live = 0;
        let n = self.slots.len();
        for i in 0..n {
            self.slots[i].in_use = false;
            self.slots[i].next_free = if i + 1 < n { (i + 1) as i32 } else { -1 };
        }
        self.free_head = 0;
        // `stability` is deliberately NOT reset: it only has to be unique and
        // increasing for as long as the process runs, and resetting it would
        // let a post-clear event sort ahead of nothing in particular.
    }

    fn alloc_slot(&mut self) -> Option<usize> {
        if self.free_head < 0 {
            return None;
        }
        let slot = self.free_head as usize;
        debug_assert!(slot < self.slots.len());
        self.free_head = self.slots[slot].next_free;
        self.live += 1;
        Some(slot)
    }

    fn free_slot(&mut self, slot: usize) {
        if !self.slots[slot].in_use {
            return;
        }
        self.slots[slot].in_use = false;
        self.slots[slot].pending = false;
        self.slots[slot].size = 0;
        self.slots[slot].next_free = self.free_head;
        self.free_head = slot as i32;
        self.live -= 1;
        if self.live == 0 {
            self.data_head = 0; // pool empty — reclaim everything, free
        }
    }

    // ── The heap ────────────────────────────────────────────────────────────

    fn heap_push(&mut self, e: Entry) {
        let mut i = self.queue_size;
        debug_assert!(i < self.queue.len());
        self.queue[i] = e;
        self.queue_size += 1;
        while i > 0 {
            let p = (i - 1) / 2;
            if self.queue[i].earlier_than(&self.queue[p]) {
                self.queue.swap(i, p);
                i = p;
            } else {
                break;
            }
        }
    }

    fn heap_pop(&mut self) {
        if self.queue_size == 0 {
            return;
        }
        self.queue_size -= 1;
        let last = self.queue_size;
        self.queue[0] = self.queue[last];
        self.sift_down(0);
    }

    fn sift_down(&mut self, mut i: usize) {
        loop {
            let (l, r) = (2 * i + 1, 2 * i + 2);
            let mut m = i;
            if l < self.queue_size && self.queue[l].earlier_than(&self.queue[m]) {
                m = l;
            }
            if r < self.queue_size && self.queue[r].earlier_than(&self.queue[m]) {
                m = r;
            }
            if m == i {
                return;
            }
            self.queue.swap(i, m);
            i = m;
        }
    }

    fn rebuild_heap(&mut self) {
        self.queue_size = 0;
        for i in 0..self.slots.len() {
            if self.slots[i].in_use && self.slots[i].pending {
                let e = Entry {
                    when: self.slots[i].when,
                    stability: self.slots[i].stability,
                    slot: i as i32,
                };
                let k = self.queue_size;
                self.queue[k] = e;
                self.queue_size += 1;
            }
        }
        if self.queue_size > 1 {
            for i in (0..self.queue_size / 2).rev() {
                self.sift_down(i);
            }
        }
    }

    // ── The data pool ───────────────────────────────────────────────────────

    /// Slide the live chunks down over the holes freed slots left, without
    /// waiting for the queue to drain. No allocation: the working set is the
    /// member scratch buffer and the sort is in place.
    fn compact(&mut self) {
        if self.live == 0 {
            self.data_head = 0;
            return;
        }
        let mut n = 0usize;
        for i in 0..self.slots.len() {
            if self.slots[i].in_use && self.slots[i].size > 0 {
                self.scratch[n] =
                    Live { off: self.slots[i].offset, size: self.slots[i].size, slot: i as i32 };
                n += 1;
            }
        }
        // Offsets are unique across live chunks (each occupies at least four
        // bytes), so this order is total and does not depend on the sort being
        // stable. `sort_unstable_by_key` allocates nothing.
        self.scratch[..n].sort_unstable_by_key(|l| l.off);

        let mut head: u64 = 0;
        for k in 0..n {
            let l = self.scratch[k];
            let aligned = (l.size as u64 + 3) & !3;
            if head != l.off as u64 {
                let from = l.off as usize;
                let to = head as usize;
                self.data.copy_within(from..from + l.size as usize, to);
                self.slots[l.slot as usize].offset = head as u32;
            }
            head += aligned;
        }
        self.data_head = head as u32;
    }
}

// No `Send`/`Sync` is claimed. The store is reached only through the C ABI, by
// raw pointer, and the sharing rule is the one the C++ header states rather
// than one the type system can express: the owning thread calls everything,
// and any other thread may call `request_clear` and nothing else. A blanket
// `Sync` would promise more than that — `next_time` and `size` read fields the
// owner is free to be mutating.

#[cfg(test)]
mod tests {
    use super::*;

    /// The store's metadata is opaque bytes it never reads, so the tests give
    /// it a `u32` — exactly what clockwork's own `EngineMeta` is — and read
    /// it back through the pointer `add` hands out.
    struct S(Store);

    impl S {
        fn new(slots: u32, pool: u32) -> S {
            S(Store::new(slots, pool, 4, 4).expect("store"))
        }
        fn add(&mut self, when: i64, tag: u32, meta: u32, payload: &[u8]) -> bool {
            let Some(m) = self.0.add(when, tag, payload) else { return false };
            m[..4].copy_from_slice(&meta.to_ne_bytes());
            true
        }
        /// Pop one due event and return `(when, tag, meta, payload)`, releasing
        /// its slot — the popDue/release pair the fire loop performs.
        fn pop(&mut self, now: i64) -> Option<(i64, u32, u32, Vec<u8>)> {
            let e = self.0.pop_due(now)?;
            let meta = u32::from_ne_bytes(e.meta[..4].try_into().unwrap());
            let (when, tag, slot, bytes) = (e.when, e.tag, e.slot, e.data.to_vec());
            self.0.release(slot);
            Some((when, tag, meta, bytes))
        }
        fn drain(&mut self, now: i64) -> Vec<(i64, u32, u32, Vec<u8>)> {
            let mut v = Vec::new();
            while let Some(e) = self.pop(now) {
                v.push(e);
            }
            v
        }
    }

    const D: &[u8] = &[1, 2, 3, 4];
    const KEEP: u32 = crate::tag::tag_hash(b"keep");
    const FLUSHME: u32 = crate::tag::tag_hash(b"flushme");

    #[test]
    fn out_of_order_insertion_pops_in_time_order_fifo_within_a_timetag() {
        let mut s = S::new(16, 8192);
        // Deliberately scrambled, with a repeated timetag in the middle.
        assert!(s.add(300, KEEP, 3, D));
        assert!(s.add(100, KEEP, 1, D));
        assert!(s.add(100, KEEP, 2, D)); // same time as 1, added later
        assert!(s.add(200, KEEP, 4, D));
        let order: Vec<u32> = s.drain(i64::MAX).iter().map(|e| e.2).collect();
        assert_eq!(order, vec![1, 2, 4, 3]);
    }

    #[test]
    fn a_long_scrambled_insertion_still_pops_sorted() {
        // A heap with one wrong comparison passes the four-event case above and
        // fails this one.
        let mut s = S::new(512, 1 << 16);
        let mut x: i64 = 1;
        let mut expect = Vec::new();
        for i in 0..400 {
            x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            let when = (x >> 40) & 0xFFFF; // heavy repetition, so FIFO matters
            assert!(s.add(when, KEEP, i, D));
            expect.push((when, i));
        }
        expect.sort_by_key(|&(w, i)| (w, i)); // stable order == insertion order
        let got: Vec<(i64, u32)> = s.drain(i64::MAX).iter().map(|e| (e.0, e.2)).collect();
        assert_eq!(got, expect);
    }

    #[test]
    fn an_event_exactly_at_now_is_due_and_one_tick_later_is_not() {
        let mut s = S::new(16, 8192);
        assert_eq!(s.0.next_time(), i64::MAX);
        assert!(s.add(500, KEEP, 7, D));
        assert_eq!(s.0.next_time(), 500);

        assert!(s.pop(499).is_none(), "not yet due");
        assert_eq!(s.0.size(), 1, "and still live");

        let (when, tag, meta, _) = s.pop(500).expect("due AT its timetag");
        assert_eq!((when, tag, meta), (500, KEEP, 7));
        assert_eq!(s.0.size(), 0);
        assert_eq!(s.0.next_time(), i64::MAX);
    }

    #[test]
    fn now_at_the_extremes_is_still_a_boundary_not_a_special_case() {
        let mut s = S::new(16, 8192);
        assert!(s.add(i64::MIN, KEEP, 1, D));
        assert!(s.add(i64::MAX, KEEP, 2, D));
        assert!(s.pop(i64::MIN).is_some());
        assert!(s.pop(i64::MAX - 1).is_none());
        assert!(s.pop(i64::MAX).is_some());
    }

    #[test]
    fn flush_cancels_exactly_one_tag_and_zero_cancels_everything() {
        let mut s = S::new(16, 8192);
        assert!(s.add(100, KEEP, 0, D));
        assert!(s.add(200, FLUSHME, 0, D));
        assert!(s.add(300, KEEP, 0, D));
        assert_eq!(s.0.size(), 3);

        s.0.flush(FLUSHME);
        assert_eq!(s.0.size(), 2);
        assert_eq!(s.0.next_time(), 100);
        assert_eq!(s.drain(i64::MAX).len(), 2, "the flushed event never surfaces");

        assert!(s.add(100, KEEP, 0, D));
        assert!(s.add(200, FLUSHME, 0, D));
        s.0.flush(0);
        assert_eq!(s.0.size(), 0);
        assert_eq!(s.0.next_time(), i64::MAX);
        assert_eq!(s.0.data_used(), 0);
    }

    #[test]
    fn tags_do_not_flush_each_other() {
        use crate::tag::{tag_hash, TAG_DEFAULT, TAG_SYNTH};
        // A tag of the caller's own, to pin that flushing is keyed on the
        // opaque value and not on a name the store knows.
        const CALLER: u32 = tag_hash(b"anything at all");
        let mut s = S::new(16, 8192);
        assert!(s.add(100, TAG_DEFAULT, 0, D));
        assert!(s.add(200, CALLER, 0, D));
        assert!(s.add(300, TAG_SYNTH, 0, D));
        s.0.flush(TAG_DEFAULT);
        assert_eq!(s.0.size(), 2);
        assert_eq!(s.0.next_time(), 200, "the caller-tagged event survives");
        s.0.flush(0);
        assert_eq!(s.0.size(), 0);
    }

    #[test]
    fn flushing_buried_future_events_does_not_leak_capacity() {
        // The regression a tombstone heap fails: a flushed event later than a
        // live one never reaches the heap top, so its capacity is never
        // reclaimed and add() starts lying about being full.
        let mut s = S::new(4, 8192);
        assert!(s.add(10, KEEP, 0, D));
        for i in 0..1000 {
            assert!(s.add(1000 + i, FLUSHME, 0, D));
            s.0.flush(FLUSHME);
            assert_eq!(s.0.size(), 1);
        }
        assert!(s.add(20, KEEP, 0, D));
        assert_eq!(s.0.size(), 2);
    }

    #[test]
    fn flush_leaves_the_survivors_in_time_order() {
        // flush() rebuilds the heap from the pool, in slot order, which has
        // nothing to do with time order — so this is where a missing heapify
        // shows up.
        let mut s = S::new(64, 1 << 16);
        for i in 0..40i64 {
            let tag = if i % 3 == 0 { FLUSHME } else { KEEP };
            assert!(s.add(1000 - i, tag, i as u32, D));
        }
        s.0.flush(FLUSHME);
        let whens: Vec<i64> = s.drain(i64::MAX).iter().map(|e| e.0).collect();
        let mut sorted = whens.clone();
        sorted.sort_unstable();
        assert_eq!(whens, sorted);
        assert!(whens.iter().all(|w| (1000 - w) % 3 != 0));
    }

    #[test]
    fn the_clear_handshake_is_requested_once_and_consumed_once() {
        let mut s = S::new(16, 8192);
        assert!(s.add(100, KEEP, 0, D));
        assert!(s.add(200, FLUSHME, 0, D));
        assert!(!s.0.drain_pending_clear(), "nothing requested yet");
        assert_eq!(s.0.size(), 2);

        s.0.request_clear();
        assert_eq!(s.0.size(), 2, "the request alone changes nothing");
        assert!(s.0.drain_pending_clear());
        assert_eq!(s.0.size(), 0);
        assert_eq!(s.0.next_time(), i64::MAX);
        assert_eq!(s.0.data_used(), 0);
        assert!(!s.0.drain_pending_clear(), "the flag is consumed, not sticky");
    }

    #[test]
    fn a_full_slot_pool_refuses_and_freeing_one_reopens_exactly_one() {
        let mut s = S::new(4, 8192);
        for i in 0..4 {
            assert!(s.add(100 + i, KEEP, 0, &[0]));
        }
        assert!(s.0.full());
        assert_eq!(s.0.size(), 4);
        assert!(!s.add(200, KEEP, 0, &[0]));
        assert_eq!(s.0.size(), 4, "a refused add changes nothing");

        assert!(s.pop(i64::MAX).is_some());
        assert!(!s.0.full());
        assert!(s.add(200, KEEP, 0, &[0]));
        assert!(!s.add(201, KEEP, 0, &[0]), "exactly one");
    }

    #[test]
    fn a_payload_larger_than_the_whole_pool_is_refused_outright() {
        let mut s = S::new(16, 256);
        assert_eq!(s.0.data_capacity(), 256);
        assert!(!s.add(100, KEEP, 0, &[0u8; 300]));
        assert_eq!(s.0.size(), 0);
        assert_eq!(s.0.data_used(), 0, "refused before anything was reserved");
        assert!(s.add(100, KEEP, 0, &[0u8; 256]), "exactly the capacity fits");
        assert_eq!(s.0.data_used(), 256);
    }

    #[test]
    fn the_data_pool_fills_while_slots_stay_free_and_full_misses_it() {
        let mut s = S::new(64, 256);
        let mut added = 0i64;
        while s.add(1000 + added, KEEP, 0, &[0u8; 64]) {
            added += 1;
        }
        assert!(added < 64, "ran out of data pool, not slots");
        assert!(!s.0.full(), "full() is slot-count only");
        assert!(!s.add(2000, KEEP, 0, &[0]), "not even one byte fits");
    }

    #[test]
    fn compaction_reclaims_holes_under_a_pinned_queue() {
        // One event pinned at the far end means the pool never drains, so the
        // ONLY way churn can continue is in-place compaction.
        let mut s = S::new(512, 524288);
        assert!(s.add(i64::MAX, KEEP, 0, &[0u8; 64]));
        for i in 0..10_000i64 {
            assert!(s.add(i, KEEP, 0, &[0xABu8; 152]), "exhausted at {i}");
            assert!(s.pop(i64::MAX).is_some());
        }
        assert_eq!(s.0.size(), 1);
    }

    #[test]
    fn compaction_moves_bytes_without_corrupting_them() {
        let mut s = S::new(16, 8192);
        assert!(s.add(i64::MAX, KEEP, 0, &[0u8; 32]));
        for i in 0..100u32 {
            let marker: Vec<u8> =
                (0..64u32).map(|b| ((i * 7 + b) & 0xFF) as u8).collect();
            assert!(s.add(i as i64, KEEP, i, &marker));
            let (_, _, meta, bytes) = s.pop(i64::MAX).expect("due");
            assert_eq!(meta, i);
            assert_eq!(bytes, marker, "payload survived the churn");
        }
    }

    #[test]
    fn compaction_preserves_every_live_payload_not_just_the_next_one() {
        // Several live chunks, holes punched between them, then a compaction
        // forced by an add that would not otherwise fit.
        let mut s = S::new(32, 1024);
        let body = |n: u8| vec![n; 100];
        for i in 0..8u8 {
            assert!(s.add(1000 + i as i64, if i % 2 == 0 { KEEP } else { FLUSHME }, i as u32, &body(i)));
        }
        s.0.flush(FLUSHME); // four holes, interleaved with four survivors
        assert!(s.add(2000, KEEP, 99, &body(99)), "compaction makes room");
        for (_, _, meta, bytes) in s.drain(i64::MAX) {
            assert_eq!(bytes, body(meta as u8), "chunk {meta} intact after the slide");
        }
    }

    #[test]
    fn a_drained_queue_resets_the_data_head() {
        let mut s = S::new(16, 8192);
        for i in 0..5 {
            assert!(s.add(i, KEEP, 0, &[1u8; 8]));
        }
        assert!(s.0.data_used() > 0);
        s.drain(i64::MAX);
        assert_eq!(s.0.data_used(), 0);
    }

    #[test]
    fn an_impossible_shape_is_refused_rather_than_built() {
        assert!(Store::new(0, 8192, 4, 4).is_none(), "no slots");
        assert!(Store::new(16, 0, 4, 4).is_none(), "no data pool");
        assert!(Store::new(MAX_SLOT_COUNT + 1, 8192, 4, 4).is_none(), "past the index width");
        assert!(Store::new(16, 8192, 4, 3).is_none(), "alignment not a power of two");
        assert!(Store::new(16, 8192, 6, 4).is_none(), "size not a multiple of alignment");
        assert!(Store::new(16, 8192, 0, 1).is_none(), "a zero-size record");
        assert!(Store::new(MAX_SLOT_COUNT, 8192, 4, 4).is_some(), "the limit itself is fine");
    }

    #[test]
    fn releasing_the_same_slot_twice_or_a_nonsense_slot_is_a_no_op() {
        let mut s = S::new(4, 8192);
        assert!(s.add(1, KEEP, 0, D));
        let slot = s.0.pop_due(i64::MAX).expect("due").slot;
        s.0.release(slot);
        assert_eq!(s.0.size(), 0);
        s.0.release(slot); // again
        s.0.release(-1);
        s.0.release(9999);
        assert_eq!(s.0.size(), 0);
        assert!(s.add(2, KEEP, 0, D), "the free list is intact");
    }

    #[test]
    fn a_zero_length_payload_is_stored_and_returned() {
        let mut s = S::new(4, 64);
        assert!(s.add(5, KEEP, 42, &[]));
        let (when, _, meta, bytes) = s.pop(i64::MAX).expect("due");
        assert_eq!((when, meta), (5, 42));
        assert!(bytes.is_empty());
    }

    #[test]
    fn stability_is_not_reset_by_a_clear() {
        // Two events added either side of a clear must still order FIFO.
        let mut s = S::new(16, 8192);
        assert!(s.add(1, KEEP, 1, D));
        s.0.clear();
        assert!(s.add(7, KEEP, 2, D));
        assert!(s.add(7, KEEP, 3, D));
        let order: Vec<u32> = s.drain(i64::MAX).iter().map(|e| e.2).collect();
        assert_eq!(order, vec![2, 3]);
    }
}
