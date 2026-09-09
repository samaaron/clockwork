// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The stable-slot table: stable slots, and how a closed one is made safe
//! without a lock.
//!
//! # Why this is its own crate
//!
//! It was written once, for `clockwork-ports`, and then the event sinks
//! turned out to want exactly the same thing: a handle that stays valid for
//! the life of what is in the slot, a close that takes effect at once on a
//! control thread while the audio thread is inside, and no lock anywhere on
//! the read path. `src/clockwork_ports.h` and `src/clockwork_event_sink.h` each state that
//! rule in their own words, and the rule is the same rule.
//!
//! Two copies of a lock-free lifetime discipline is one copy too many — the
//! second is the one that quietly loses a subtlety and takes a year to find
//! out. So the table moved here, generic over what sits in the slot, and both
//! disciplines stand on it. This crate is deliberately the whole of what they
//! share: the ring, the policy for not keeping up, and the endpoint are all
//! different between frames and events, and none of them is here.
//!
//! # The problem
//!
//! A slot goes silent the instant it is closed, a closed slot reads as nothing
//! rather than faulting, and it is not reused "until every reader has been
//! told". Closing happens on a control thread; reading happens on the audio
//! thread; and the memory a reader is walking is the memory a close would like
//! to free. A mutex would solve it and is exactly what may not appear on the
//! read path.
//!
//! # The answer: one word per slot
//!
//! Every slot carries a single `AtomicU64`:
//!
//! ```text
//!   63          48 47        32 31                              0
//!  +--------------+------------+---------------------------------+
//!  |  generation  |   state    |            refcount             |
//!  +--------------+------------+---------------------------------+
//! ```
//!
//! `refcount` occupies the LOW bits, so entering a slot is `word + 1` and
//! leaving is `word - 1` — one atomic each, with the state and generation
//! carried along untouched. A `u32` refcount cannot overflow into `state`
//! before the machine runs out of threads.
//!
//! * **Enter** ([`SlotTable::acquire`], every audio-thread and endpoint call)
//!   is a compare-exchange loop: read the word; refuse unless `state == OPEN`
//!   and the generation matches the handle; then CAS in `word + 1`. A CAS that
//!   loses only ever loses to another enter or leave, so it is not a contended
//!   loop.
//! * **Leave** is `fetch_sub(1)`, in [`Held`]'s `Drop`.
//! * **Close** is a CAS from `OPEN` to `CLOSED`, preserving the refcount.
//!   After it lands no new enter can succeed, so the population of readers
//!   only falls.
//! * **Reclaim** — the "genuinely free" test — is a CAS from
//!   `{state: CLOSED, refcount: 0}` to `{state: BUSY, refcount: 0}`. It can
//!   only succeed when the state says no new reader may enter AND the count
//!   says no old reader is still inside, and the two are read as one word so
//!   they cannot disagree. The thread that wins that CAS owns the slot
//!   exclusively: it drops the old occupant (freeing whatever it held), then
//!   publishes `FREE`. That is the whole of it — no lock, no epoch list, no
//!   hazard pointer, and no deferred-free thread.
//!
//! Reclaim is attempted by `close` (the common case: the audio thread is not
//! inside, so it frees immediately) and again by every `open` as it sweeps for
//! a slot. An entry closed while a reader happened to be inside stays
//! `CLOSED`, costing its memory, until the next `open` sweeps past it. Bounded
//! by the table size, and never a correctness problem.
//!
//! # The generation is not decoration
//!
//! A handle is `(generation << 16) | (slot + 1)`. Without the generation, a
//! handle held across a close-and-reopen would address the new occupant on a
//! slot the holder thinks is still its own — which is precisely the
//! renumbering fault both headers forbid, arriving by the other door. With it,
//! the stale handle fails the enter test and reads as closed. The generation
//! is 16 bits, so a slot must be reopened 65536 times before a handle from the
//! first of those could alias; nothing here opens at that rate, and the
//! alternative is a 64-bit handle neither header has.

use core::sync::atomic::{AtomicPtr, Ordering};
// 64-bit atomics from portable-atomic: native where the ISA has them, a
// critical section on Xtensa — where the table is only ever touched from one
// thread anyway.
use portable_atomic::AtomicU64;

/// 0 is never a valid handle in either discipline, so it doubles as "none".
pub const HANDLE_NONE: u32 = 0;

const STATE_FREE: u64 = 0;
const STATE_OPEN: u64 = 1;
const STATE_CLOSED: u64 = 2;
const STATE_BUSY: u64 = 3;

const REFS_MASK: u64 = 0xffff_ffff;
const STATE_SHIFT: u32 = 32;
const STATE_MASK: u64 = 0xffff << STATE_SHIFT;
const GEN_SHIFT: u32 = 48;

#[inline] fn refs(w: u64) -> u64 { w & REFS_MASK }
#[inline] fn state(w: u64) -> u64 { (w & STATE_MASK) >> STATE_SHIFT }
#[inline] fn generation(w: u64) -> u64 { w >> GEN_SHIFT }
#[inline] fn pack(gen: u64, st: u64, rc: u64) -> u64 {
    (gen << GEN_SHIFT) | (st << STATE_SHIFT) | rc
}

/// The observable states of a slot. Only ever asked for by a diagnostic or a
/// test — the whole close-under-a-reader rule is a statement about this word,
/// and a test that could only observe it through symptoms would be a test of
/// the allocator's habits rather than of the rule.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SlotState {
    /// Nothing in it, and nothing is coming back.
    Free,
    /// Occupied and enterable.
    Open,
    /// Closed to new readers, still holding its memory for one that is inside.
    Closed,
    /// Momentarily owned by an opener or a reclaimer. Never observed for long.
    Busy,
}

struct Slot<T> {
    word: AtomicU64,
    occupant: AtomicPtr<T>,
}

impl<T> Slot<T> {
    const fn new() -> Slot<T> {
        Slot { word: AtomicU64::new(0), occupant: AtomicPtr::new(core::ptr::null_mut()) }
    }
}

/// A fixed table of `N` slots holding `T`.
///
/// Meant to live in a `static`: no allocation, no initialisation order,
/// nothing to boot. The table exists before main and outlives it — which is
/// what lets a C ABI hand out bare `u32` handles with no context pointer, as
/// both headers do.
pub struct SlotTable<T, const N: usize> {
    slots: [Slot<T>; N],
}

/// A live reference to a slot's occupant, with the refcount held for its
/// lifetime. While one of these exists the slot cannot be reclaimed, so the
/// `T` behind it cannot be freed.
pub struct Held<'a, T> {
    slot: &'a Slot<T>,
    occupant: &'a T,
}

impl<T> Held<'_, T> {
    #[inline]
    pub fn get(&self) -> &T { self.occupant }
}

impl<T> core::ops::Deref for Held<'_, T> {
    type Target = T;
    #[inline]
    fn deref(&self) -> &T { self.occupant }
}

impl<T> Drop for Held<'_, T> {
    #[inline]
    fn drop(&mut self) {
        self.slot.word.fetch_sub(1, Ordering::Release);
    }
}

impl<T, const N: usize> Default for SlotTable<T, N> {
    fn default() -> Self { Self::new() }
}

impl<T, const N: usize> SlotTable<T, N> {
    /// A table of empty slots, buildable in a `static`.
    pub const fn new() -> Self {
        // N must fit the handle's 16-bit index field. A const assert rather
        // than a runtime one: a table too large is a compile-time mistake.
        assert!(N > 0 && N <= 0xffff);
        SlotTable { slots: [const { Slot::new() }; N] }
    }

    /// How many slots there are, occupied or not.
    pub const fn capacity(&self) -> usize { N }

    #[inline]
    fn decode(&self, handle: u32) -> Option<(usize, u64)> {
        let idx = (handle & 0xffff) as usize;
        if idx == 0 || idx > N { return None; }
        Some((idx - 1, (handle >> 16) as u64))
    }

    /// The slot index a handle names, ignoring whether it is still live.
    /// Diagnostics and tests: "did the reopen come back to the same slot".
    pub fn slot_index(&self, handle: u32) -> Option<usize> {
        self.decode(handle).map(|(idx, _)| idx)
    }

    /// Take a reference on `handle`'s occupant, or `None` if the handle is
    /// stale, the slot is closed, or the handle was never valid.
    /// Allocation-free and wait-free but for a CAS retry.
    pub fn acquire(&self, handle: u32) -> Option<Held<'_, T>> {
        let (idx, gen) = self.decode(handle)?;
        let slot = &self.slots[idx];
        loop {
            let w = slot.word.load(Ordering::Acquire);
            if state(w) != STATE_OPEN || generation(w) != gen {
                return None;
            }
            // `w + 1` carries state and generation through untouched; if
            // either changed under us the CAS fails and we re-read.
            match slot.word.compare_exchange_weak(
                w, w + 1, Ordering::Acquire, Ordering::Relaxed,
            ) {
                Ok(_) => break,
                Err(_) => continue,
            }
        }
        let p = slot.occupant.load(Ordering::Acquire);
        if p.is_null() {
            // Cannot happen — OPEN is published after the pointer — but a null
            // deref on the audio thread is not the place to find out.
            slot.word.fetch_sub(1, Ordering::Release);
            return None;
        }
        // SAFETY: the occupant is published before OPEN and never dropped
        // while a refcount is held, and this slot's count was just taken.
        Some(Held { slot, occupant: unsafe { &*p } })
    }

    /// If the slot is closed and nobody is inside, take it, drop its occupant,
    /// and mark it free. Returns true if it did.
    ///
    /// Public because both `close` and `open` drive it, and because a test
    /// that means to assert "now, and only now, is it genuinely free" has to
    /// be able to ask.
    pub fn try_reclaim(&self, idx: usize) -> bool {
        if idx >= N { return false; }
        let slot = &self.slots[idx];
        let w = slot.word.load(Ordering::Acquire);
        if state(w) != STATE_CLOSED || refs(w) != 0 {
            return false;
        }
        let gen = generation(w);
        if slot.word.compare_exchange(
            w, pack(gen, STATE_BUSY, 0), Ordering::AcqRel, Ordering::Relaxed,
        ).is_err() {
            return false;
        }
        // Exclusive: state is BUSY, so no enter can succeed and no other
        // reclaim can win the same CAS.
        let old = slot.occupant.swap(core::ptr::null_mut(), Ordering::AcqRel);
        if !old.is_null() {
            // SAFETY: the box `open` leaked; BUSY with zero refs makes this
            // the only reclaimer and leaves nobody inside.
            unsafe { drop(Box::from_raw(old)) };
        }
        slot.word.store(pack(gen, STATE_FREE, 0), Ordering::Release);
        true
    }

    /// Put something in a free slot and return its handle, or [`HANDLE_NONE`]
    /// if the table is full.
    ///
    /// `make` runs BEFORE the sweep, so whatever allocation the occupant needs
    /// happens outside the window where a slot is BUSY — and is simply dropped
    /// on the caller's thread if there was no room. Control thread only.
    pub fn open(&self, make: impl FnOnce() -> T) -> u32 {
        let mut boxed = Some(Box::new(make()));

        for idx in 0..N {
            // Sweep: a slot closed while a reader was inside becomes free here.
            self.try_reclaim(idx);

            let slot = &self.slots[idx];
            let w = slot.word.load(Ordering::Acquire);
            if state(w) != STATE_FREE || refs(w) != 0 { continue; }
            let gen = generation(w);
            if slot.word.compare_exchange(
                w, pack(gen, STATE_BUSY, 0), Ordering::AcqRel, Ordering::Relaxed,
            ).is_err() {
                continue;   // another opener took it
            }

            // Exclusive. Install, then bump the generation, then publish OPEN —
            // the Release store is what makes the pointer visible to a reader
            // that acquires the word.
            let p = Box::into_raw(boxed.take().expect("occupant box consumed twice"));
            let stale = slot.occupant.swap(p, Ordering::AcqRel);
            // SAFETY: a leftover nobody can reach (the slot was FREE, with no
            // refs); the box `open` leaked, freed once.
            if !stale.is_null() { unsafe { drop(Box::from_raw(stale)) }; }

            let next_gen = (gen + 1) & 0xffff;
            let next_gen = if next_gen == 0 { 1 } else { next_gen };
            slot.word.store(pack(next_gen, STATE_OPEN, 0), Ordering::Release);
            return ((next_gen as u32) << 16) | ((idx + 1) as u32);
        }
        // Table full. `boxed` drops here, on the control thread.
        HANDLE_NONE
    }

    /// Close a slot: shut to new readers immediately, freed as soon as nobody
    /// is inside. Idempotent, and safe against a stale or never-valid handle.
    pub fn close(&self, handle: u32) {
        let Some((idx, gen)) = self.decode(handle) else { return };
        let slot = &self.slots[idx];
        loop {
            let w = slot.word.load(Ordering::Acquire);
            if generation(w) != gen || state(w) != STATE_OPEN {
                return;     // already closed, reclaimed, or never ours
            }
            if slot.word.compare_exchange_weak(
                w, pack(gen, STATE_CLOSED, refs(w)),
                Ordering::AcqRel, Ordering::Relaxed,
            ).is_ok() {
                break;
            }
            // Lost to an enter or a leave changing the refcount; retry.
        }
        // The common case: nothing was inside, so the memory goes back now.
        self.try_reclaim(idx);
    }

    /// Every open slot's handle, in slot order. Writes at most `cap` through
    /// `out`, and returns how many exist — which may exceed `cap`. No
    /// allocation, so it is safe from the audio thread.
    ///
    /// # Safety
    /// `out` is valid for `cap` writes, or null when `cap` is 0.
    pub unsafe fn list_raw(&self, out: *mut u32, cap: u32) -> u32 {
        let mut n: u32 = 0;
        for (idx, slot) in self.slots.iter().enumerate() {
            let w = slot.word.load(Ordering::Acquire);
            if state(w) != STATE_OPEN { continue; }
            if n < cap && !out.is_null() {
                let h = ((generation(w) as u32) << 16) | ((idx + 1) as u32);
                // SAFETY: `n < cap`, and `out` is valid for `cap` writes per
                // the contract.
                unsafe { *out.add(n as usize) = h };
            }
            n += 1;
        }
        n
    }

    /// Every open slot's handle, into a slice. The safe form of
    /// [`Self::list_raw`], for Rust callers.
    pub fn list(&self, out: &mut [u32]) -> u32 {
        // SAFETY: `out.len()` writable entries at `out`.
        unsafe { self.list_raw(out.as_mut_ptr(), out.len() as u32) }
    }

    /// Close every open slot. For shutdown and for tests, so one case cannot
    /// leak slots into the next.
    pub fn close_all(&self) {
        for (idx, slot) in self.slots.iter().enumerate() {
            let w = slot.word.load(Ordering::Acquire);
            if state(w) == STATE_OPEN {
                let h = ((generation(w) as u32) << 16) | ((idx + 1) as u32);
                self.close(h);
            }
        }
    }

    /// The raw state of the slot a handle names, ignoring the generation.
    /// Diagnostics: see [`SlotState`].
    pub fn slot_state(&self, handle: u32) -> Option<SlotState> {
        let (idx, _) = self.decode(handle)?;
        Some(match state(self.slots[idx].word.load(Ordering::Acquire)) {
            STATE_FREE => SlotState::Free,
            STATE_OPEN => SlotState::Open,
            STATE_CLOSED => SlotState::Closed,
            _ => SlotState::Busy,
        })
    }

    /// How many readers are inside the slot a handle names. Diagnostics.
    pub fn slot_refs(&self, handle: u32) -> Option<u64> {
        let (idx, _) = self.decode(handle)?;
        Some(refs(self.slots[idx].word.load(Ordering::Acquire)))
    }
}

// SAFETY: the word is what synchronises: an occupant is published with a
// Release store and read after an Acquire load, and it is never dropped while
// a refcount is held. That makes the table shareable exactly when the
// occupant is.
unsafe impl<T: Send + Sync, const N: usize> Sync for SlotTable<T, N> {}
// SAFETY: as above; moving the table moves boxes, which is what `T: Send`
// permits.
unsafe impl<T: Send, const N: usize> Send for SlotTable<T, N> {}

#[cfg(test)]
mod tests {
    use super::*;

    struct Thing { tag: u32 }

    static TABLE: SlotTable<Thing, 8> = SlotTable::new();

    // The table is process-global, exactly as it is in the crates that use it,
    // so cases must not leak into each other. cargo runs them on threads of
    // one process, so this serialises them rather than pretending otherwise.
    static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    #[test]
    fn a_slot_with_nobody_inside_is_freed_the_moment_it_is_closed() {
        let _g = LOCK.lock().unwrap();
        TABLE.close_all();
        let h = TABLE.open(|| Thing { tag: 1 });
        assert_ne!(h, HANDLE_NONE);
        assert_eq!(TABLE.slot_state(h), Some(SlotState::Open));
        TABLE.close(h);
        // Not merely marked closed: reclaimed, its memory given back.
        // Deferring that to the next open would mean a closed slot held its
        // memory for as long as nobody opened another.
        assert_eq!(TABLE.slot_state(h), Some(SlotState::Free));
    }

    #[test]
    fn a_closed_slot_stays_reserved_while_a_reader_is_inside_it() {
        let _g = LOCK.lock().unwrap();
        TABLE.close_all();
        // The property the whole one-word slot design exists for, tested
        // directly rather than through a race that may or may not happen.
        // (Established by mutation: a stress test that closed slots under a
        // live reader thread twenty thousand times did NOT notice the refcount
        // being dropped from the reclaim condition, because the freed occupant
        // and its replacement come from the same allocator block and look
        // alike. This does, every time.)
        let h = TABLE.open(|| Thing { tag: 2 });
        assert_ne!(h, HANDLE_NONE);
        let held = TABLE.acquire(h).expect("an open slot must be enterable");
        assert_eq!(TABLE.slot_refs(h), Some(1));

        TABLE.close(h);
        // Closed to new readers immediately — that is "goes silent at once".
        assert!(TABLE.acquire(h).is_none(), "a closed slot must not admit a reader");
        // But NOT free: the reader inside is still walking it, and reclaiming
        // here is a use-after-free on the audio thread.
        assert_eq!(TABLE.slot_state(h), Some(SlotState::Closed));
        assert_eq!(TABLE.slot_refs(h), Some(1));
        assert_eq!(held.tag, 2, "the occupant behind the guard is intact");

        // A slot in that state must not be handed to anyone else either.
        let other = TABLE.open(|| Thing { tag: 3 });
        assert_ne!(other, HANDLE_NONE);
        assert_ne!(TABLE.slot_index(other), TABLE.slot_index(h),
                   "a slot with a reader inside was reused");

        drop(held);
        assert_eq!(TABLE.slot_refs(h), Some(0));
        // Now, and only now, is it genuinely free — swept by the next open.
        assert!(TABLE.try_reclaim(TABLE.slot_index(h).unwrap()));
        assert_eq!(TABLE.slot_state(h), Some(SlotState::Free));
        TABLE.close(other);
    }

    #[test]
    fn a_stale_handle_cannot_enter_the_slot_it_used_to_name() {
        let _g = LOCK.lock().unwrap();
        TABLE.close_all();
        // Without the generation, a handle held across a close and a reopen
        // would address the new occupant on a slot its holder believes is
        // still its own.
        let first = TABLE.open(|| Thing { tag: 4 });
        assert_ne!(first, HANDLE_NONE);
        let idx = TABLE.slot_index(first).unwrap();
        TABLE.close(first);

        // Reopen until the same slot comes back (it is the lowest free one, so
        // in practice immediately).
        let mut second = HANDLE_NONE;
        for _ in 0..TABLE.capacity() + 1 {
            let h = TABLE.open(|| Thing { tag: 5 });
            assert_ne!(h, HANDLE_NONE);
            if TABLE.slot_index(h) == Some(idx) { second = h; break; }
        }
        assert_ne!(second, HANDLE_NONE, "the freed slot never came back");
        assert_ne!(second, first, "a reopened slot must not reissue the old handle");
        assert!(TABLE.acquire(first).is_none(), "a stale handle entered a live slot");
        assert!(TABLE.acquire(second).is_some());
        TABLE.close_all();
    }

    #[test]
    fn a_full_table_refuses_and_drops_what_it_could_not_place() {
        let _g = LOCK.lock().unwrap();
        TABLE.close_all();
        let mut hs = Vec::new();
        for _ in 0..TABLE.capacity() {
            let h = TABLE.open(|| Thing { tag: 6 });
            assert_ne!(h, HANDLE_NONE);
            hs.push(h);
        }
        assert_eq!(TABLE.open(|| Thing { tag: 7 }), HANDLE_NONE);
        for h in hs { TABLE.close(h); }
    }
}
