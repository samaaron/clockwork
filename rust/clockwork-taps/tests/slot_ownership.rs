// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The slot ownership rule, which is the one thing in this crate that is not
//! incidental.
//!
//! A tap slot can be re-claimed by a new unit while a superseded one is
//! still alive under an FX tail. The older unit's late destructor must not
//! mark the slot free underneath the new writer: readers gate on
//! `state == 1`, so the scope would grey out while audio kept playing, and
//! nothing would say why. The crate's own header calls this out as the part
//! that matters; these are the cases that hold it.
//!
//! # The arena
//!
//! The crate is handed a core's owner table and the arena it works in, and
//! reads the tap region's geometry from the table at the arena's front. A
//! test lays out an arena of its own: a table with one region, the taps,
//! and the region itself, laid out exactly as `shared_memory.h` describes;
//! the crate cannot tell the difference.
//!
//! Single-threaded and serialised on purpose: the cases share one arena and
//! one owner table, so cases that claimed slots concurrently would be
//! testing the test harness rather than the rule.

use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use clockwork_abi::arena::{
    geom, ClockworkArenaHeader, ClockworkArenaRegion, CLOCKWORK_ARENA_HEADER_BYTES, CLOCKWORK_ARENA_MAGIC,
};
use clockwork_taps::{
    clockwork_tap_get, clockwork_tap_release, stream_activate, stream_release, TapBufferHnd, Taps,
};

// The tap region's geometry. Small, but the same SHAPE as production: a
// global header, then fixed-size slots each holding a header and a ring.
const START: usize = CLOCKWORK_ARENA_HEADER_BYTES as usize;
const HEADER_SIZE: usize = 64;
const RING_FRAMES: u32 = 256;
const CHANNELS: u32 = 2;
const SLOT_HEADER: usize = 32;
const SLOT_SIZE: usize = SLOT_HEADER + (RING_FRAMES as usize) * (CHANNELS as usize) * 4;
const MAX_TAPS: usize = 4;
// The engine's track taps: the same slot shape, after the guest's slots in
// the same region, one index space.
const TRACK_SLOTS: usize = 2;

// An arena of a core's: its table names the tap region and nothing else.
// Leaked, so the pointer is valid for every case; 8-aligned, as the table is.
fn arena() -> *mut u8 {
    arena_with(MAX_TAPS, TRACK_SLOTS)
}

// The same with `guest` guest slots and `track` track slots, as a core's
// profile lays them out.
fn arena_with(guest: usize, track: usize) -> *mut u8 {
    let taps_bytes = HEADER_SIZE + SLOT_SIZE * (guest + track);
    let words: &'static mut [u64] = Box::leak(vec![0u64; (START + taps_bytes).div_ceil(8)].into_boxed_slice());
    let base = words.as_mut_ptr().cast::<u8>();
    // SAFETY: the buffer holds the table and is 8-aligned; zero is a valid
    // table of nothing, which the fields below fill in.
    let h = unsafe { &mut *base.cast::<ClockworkArenaHeader>() };
    h.magic = CLOCKWORK_ARENA_MAGIC;
    h.entry_count = 1;
    let e = &mut h.entries[0];
    e.id = ClockworkArenaRegion::CLOCKWORK_TAPS.0;
    e.offset = START as u32;
    e.bytes = taps_bytes as u32;
    e.geom[geom::TAP_SLOTS] = guest as u32;
    e.geom[geom::TAP_HEADER_BYTES] = HEADER_SIZE as u32;
    e.geom[geom::TAP_SLOT_BYTES] = SLOT_SIZE as u32;
    e.geom[geom::TAP_SLOT_HEADER] = SLOT_HEADER as u32;
    e.geom[geom::TAP_RING_FRAMES] = RING_FRAMES;
    e.geom[geom::TAP_CHANNELS] = CHANNELS;
    e.geom[geom::TAP_TRACK_SLOTS] = track as u32;
    e.geom[geom::TAP_TRACK_FIRST] = guest as u32;
    base
}

// The core the cases work in: its arena and its owner table.
fn base_ptr() -> *mut u8 {
    static P: OnceLock<usize> = OnceLock::new();
    *P.get_or_init(|| arena() as usize) as *mut u8
}
fn taps() -> &'static Taps {
    static T: OnceLock<Taps> = OnceLock::new();
    T.get_or_init(Taps::default)
}

// The cases share the arena and the table, so they take a lock rather than
// racing each other through them.
fn lock() -> std::sync::MutexGuard<'static, ()> {
    static L: Mutex<()> = Mutex::new(());
    L.lock().unwrap_or_else(|e| e.into_inner())
}

/// A u32 at `offset` into an arena, read as the crate reads it.
fn arena_u32(base: *mut u8, offset: usize) -> u32 {
    // SAFETY: every offset passed is inside a leaked arena, and 4-aligned:
    // the arena is 8-aligned and the layout constants are multiples of 16.
    unsafe { (*base.add(offset).cast::<AtomicU32>()).load(Ordering::Acquire) }
}
fn region_u32(offset: usize) -> u32 { arena_u32(base_ptr(), offset) }

fn slot_state_in(base: *mut u8, index: usize) -> u32 {
    // `state` is the first u32 of the slot header (shm_tap_stream). One
    // index space: the guest's slots, then the track taps, one stride apart.
    arena_u32(base, START + HEADER_SIZE + SLOT_SIZE * index)
}
fn slot_state(index: usize) -> u32 { slot_state_in(base_ptr(), index) }

/// How many times slot `index` has gone live: the u32 after `capacity_frames`
/// in the slot header.
fn slot_activations(index: usize) -> u32 {
    region_u32(START + HEADER_SIZE + SLOT_SIZE * index + 12)
}

/// The global header's active-slot counter.
fn active_count() -> u32 {
    region_u32(START + 4)
}

fn claim_in(taps: &Taps, base: *mut u8, index: usize, hnd: &mut TapBufferHnd) -> bool {
    // SAFETY: `hnd` is a live handle; `base` an arena of the test's.
    unsafe {
        clockwork_tap_get(taps, base.cast::<c_void>(), index as c_int, CHANNELS as c_int,
                          RING_FRAMES as c_int, hnd) != 0
    }
}
fn claim(index: usize, hnd: &mut TapBufferHnd) -> bool { claim_in(taps(), base_ptr(), index, hnd) }

fn release_in(taps: &Taps, base: *mut u8, hnd: &mut TapBufferHnd) {
    // SAFETY: as claim_in.
    unsafe { clockwork_tap_release(taps, base.cast::<c_void>(), hnd) }
}
fn release(hnd: &mut TapBufferHnd) { release_in(taps(), base_ptr(), hnd) }

fn handle() -> TapBufferHnd {
    // Zeroed, as a ugen's would be before its first claim.
    // SAFETY: two null pointers and two zero counts are a valid handle.
    unsafe { std::mem::zeroed() }
}

#[test]
fn a_claimed_slot_becomes_active_and_a_released_one_does_not() {
    let _g = lock();
    let mut h = handle();
    assert!(claim(0, &mut h));
    assert_eq!(slot_state(0), 1, "a claimed slot must read as active");
    assert!(!h.internalData.is_null(), "the handle must name its slot");

    release(&mut h);
    assert_eq!(slot_state(0), 0, "a released slot must read as free");
    assert!(h.internalData.is_null(), "release must null the handle");
}

#[test]
fn a_superseded_unit_cannot_free_the_slot_it_lost() {
    let _g = lock();

    // The old unit claims, and is then superseded: a re-run claims the same
    // scope number before the old one's destructor has run, which is ordinary
    // under an FX kill-delay tail.
    let mut old = handle();
    assert!(claim(1, &mut old));
    let mut new = handle();
    assert!(claim(1, &mut new));
    assert_eq!(slot_state(1), 1);

    // THE CASE. The old destructor arrives late. It still holds a handle
    // pointing at slot 1, and it must not free it.
    release(&mut old);
    assert_eq!(
        slot_state(1), 1,
        "a superseded unit freed the slot under the live writer — the scope \
         greys out while the audio plays on, which is the failure the \
         ownership rule exists to prevent"
    );

    // The live owner still can, and does.
    release(&mut new);
    assert_eq!(slot_state(1), 0);
}

#[test]
fn releasing_twice_is_harmless() {
    let _g = lock();
    let mut h = handle();
    assert!(claim(2, &mut h));
    release(&mut h);
    // The handle was nulled, so the second call has nothing to act on. A
    // destructor running twice is a real shape (a defensive teardown after an
    // explicit release), and it must not free a slot someone else has since
    // taken.
    let mut other = handle();
    assert!(claim(2, &mut other));
    release(&mut h);
    assert_eq!(slot_state(2), 1, "a double release freed a slot it no longer owned");
    release(&mut other);
}

#[test]
fn an_out_of_range_slot_is_refused_rather_than_indexed() {
    let _g = lock();
    let mut h = handle();
    assert!(!claim(MAX_TAPS + TRACK_SLOTS, &mut h), "a slot past both spans was claimed");
    assert!(h.internalData.is_null());

    let mut neg = handle();
    assert!(
        // SAFETY: `neg` is a live handle.
        unsafe {
            clockwork_tap_get(taps(), base_ptr().cast::<c_void>(), -1, CHANNELS as c_int,
                          RING_FRAMES as c_int, &mut neg) == 0
        },
        "a negative slot index was claimed"
    );
}

#[test]
fn re_claiming_a_live_slot_reformats_it_for_the_new_owner() {
    let _g = lock();
    let mut first = handle();
    assert!(claim(3, &mut first));

    // A re-used scope number re-formats rather than refusing: that is what the
    // C++ side does and what a re-run needs. The point of the case is that the
    // NEW handle owns it afterwards — otherwise the ownership check above has
    // nothing to compare against.
    let mut second = handle();
    assert!(claim(3, &mut second));
    assert_eq!(slot_state(3), 1);

    release(&mut first);
    assert_eq!(slot_state(3), 1, "the loser of the re-claim freed the winner's slot");
    release(&mut second);
    assert_eq!(slot_state(3), 0, "the winner could not free its own slot");
}

#[test]
fn a_handle_whose_slot_is_outside_the_region_touches_nothing() {
    let _g = lock();
    // A handle can outlive the region it was claimed in: the segment is
    // created and destroyed with the engine, and a scope unit under an FX
    // tail can tear down after that. Its slot pointer then lands in memory
    // the current region does not cover. A release that treated "cannot
    // identify the slot" as "free it anyway" would store 0 through that
    // pointer, into whatever lives there now.
    //
    // The decoy stands in for that memory: a slot-shaped buffer, marked
    // active, that the region does not contain.
    let mut decoy = vec![0u32; SLOT_SIZE / 4];
    decoy[0] = 1;
    let mut h = handle();
    h.internalData = decoy.as_mut_ptr().cast::<c_void>();
    let active_before = active_count();

    release(&mut h);

    assert_eq!(decoy[0], 1, "release wrote through a pointer outside the region");
    assert_eq!(
        active_count(),
        active_before,
        "the active count moved for a slot the region does not hold"
    );
    assert!(h.internalData.is_null(), "the handle was not nulled");
    assert!(h.data.is_null());
}

#[test]
fn a_track_tap_is_a_tap_slot_numbered_after_the_guests() {
    let _g = lock();
    // The first index past the guest's slots is the next slot of the same
    // region, one stride past the guest's last.
    let mut h = handle();
    assert!(claim(MAX_TAPS, &mut h), "the first track tap was refused");
    assert_eq!(h.internalData as usize, base_ptr() as usize + START + HEADER_SIZE + SLOT_SIZE * MAX_TAPS);
    assert_eq!(slot_state(MAX_TAPS), 1);
    assert_eq!(slot_state(MAX_TAPS - 1), 0, "the guest's last slot was not touched");

    // Ownership holds across the span boundary: a later claimant of the
    // same tap supersedes, and the old handle's release is a no-op.
    let mut later = handle();
    assert!(claim(MAX_TAPS, &mut later));
    release(&mut h);
    assert_eq!(slot_state(MAX_TAPS), 1, "a superseded handle freed a track tap");
    release(&mut later);
    assert_eq!(slot_state(MAX_TAPS), 0);

    // The last tap is the last slot there is.
    let mut last = handle();
    assert!(claim(MAX_TAPS + TRACK_SLOTS - 1, &mut last));
    assert!(h.internalData.is_null());
    release(&mut last);
}

#[test]
fn every_claim_counts_once_on_the_slot() {
    // A reader polls the state; a loop can claim a slot and release it between
    // two polls. The slot's count is how the reader knows it went live anyway,
    // so every way a slot is claimed must count, and nothing else may.
    let _g = lock();
    for index in [0, MAX_TAPS] {   // a guest slot and a track tap
        let before = slot_activations(index);
        let mut h = handle();
        assert!(claim(index, &mut h));
        assert_eq!(slot_activations(index), before.wrapping_add(1), "a claim must count");

        let mut again = handle();
        assert!(claim(index, &mut again));
        assert_eq!(slot_activations(index), before.wrapping_add(2), "a re-claim is another live spell");

        release(&mut h);
        release(&mut again);
        assert_eq!(slot_state(index), 0);
        assert_eq!(slot_activations(index), before.wrapping_add(2), "a release must not count");

        // A Rust writer's claim counts the same.
        // SAFETY: the test's arena, laid out with the installed geometry and never freed.
        assert!(unsafe { stream_activate(base_ptr(), index, CHANNELS) });
        assert_eq!(slot_activations(index), before.wrapping_add(3), "a Rust writer's claim must count");
        // SAFETY: as above.
        unsafe { stream_release(base_ptr(), index) };
        assert_eq!(slot_activations(index), before.wrapping_add(3));
    }
}

#[test]
fn each_core_owns_the_slots_in_its_own_arena() {
    // Two cores, each with its arena and its owner table, each claim slot 0.
    // One giving its claim back frees its own slot and leaves the other's
    // live: a table shared between them would have the second claim take
    // the first's ownership, and the first release would free nothing.
    let _g = lock();
    let (other, other_taps) = (arena(), Taps::default());
    let mut mine = handle();
    let mut theirs = handle();
    assert!(claim(0, &mut mine));
    assert!(claim_in(&other_taps, other, 0, &mut theirs));

    release(&mut mine);
    assert_eq!(slot_state(0), 0, "my slot is free");
    assert_eq!(slot_state_in(other, 0), 1, "the other core's slot is still live");
    release_in(&other_taps, other, &mut theirs);
    assert_eq!(slot_state_in(other, 0), 0);
}

// A profile with no guest tap slots still has its track taps
// (docs/ENGINES.md): the region is taps when it holds any slot.
#[test]
fn track_taps_are_there_with_no_guest_slots() {
    let _l = lock();
    let base = arena_with(0, TRACK_SLOTS);
    let taps = Taps::default();
    let mut hnd = handle();
    assert!(claim_in(&taps, base, 0, &mut hnd), "the first track tap: slot 0 of the one index space");
    assert_eq!(slot_state_in(base, 0), 1);
    release_in(&taps, base, &mut hnd);
}
