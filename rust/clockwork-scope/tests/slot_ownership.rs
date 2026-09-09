// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The slot ownership rule, which is the one thing in this crate that is not
//! incidental — and had no test.
//!
//! A scope slot can be re-claimed by a new unit while a superseded one is
//! still alive under an FX tail. The older unit's late destructor must not
//! mark the slot free underneath the new writer: readers gate on
//! `state == 1`, so the scope would grey out while audio kept playing, and
//! nothing would say why. The crate's own header calls this out as the part
//! that matters; these are the cases that hold it.
//!
//! # Standing in for the host
//!
//! The crate reaches shared memory through two symbols the host defines —
//! `clockwork_shm_base` and `clockwork_scope_geometry`. A test can define them, which is
//! why this is testable in Rust at all: the region is a plain buffer here, laid
//! out exactly as `shared_memory.h` describes, and the crate cannot tell the
//! difference.
//!
//! Single-threaded and serialised on purpose. The crate's slot table is
//! process-global (`SLOT_OWNERS`), so cases that claimed slots concurrently
//! would be testing the test harness rather than the rule.

use std::os::raw::{c_int, c_void};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};

use clockwork_scope::{clockwork_scope_get, clockwork_scope_release, ScopeBufferHnd};

// Geometry the fake host reports. Small, but the same SHAPE as production:
// a global header, then fixed-size slots each holding a header and a ring.
const START: usize = 0;
const HEADER_SIZE: usize = 64;
const RING_FRAMES: u32 = 256;
const CHANNELS: u32 = 2;
const SLOT_HEADER: usize = 32;
const SLOT_SIZE: usize = SLOT_HEADER + (RING_FRAMES as usize) * (CHANNELS as usize) * 4;
const MAX_SCOPES: usize = 4;
// The engine's track taps: a second span of the same slot shape, numbered
// after the guest's, in what would be the clockwork block. Here it simply
// follows the scope region, with a gap so the two cannot be confused for
// one contiguous array.
const TRACK_SLOTS: usize = 2;
const TRACK_START: usize = START + HEADER_SIZE + SLOT_SIZE * MAX_SCOPES + 256;
const REGION: usize = TRACK_START + SLOT_SIZE * TRACK_SLOTS;

fn region() -> &'static Mutex<Vec<u8>> {
    static R: OnceLock<Mutex<Vec<u8>>> = OnceLock::new();
    R.get_or_init(|| Mutex::new(vec![0u8; REGION]))
}

// The base pointer the crate asks for. Stable for the process: the buffer is
// leaked once so the pointer stays valid for every case.
fn base_ptr() -> *mut u8 {
    static P: OnceLock<usize> = OnceLock::new();
    *P.get_or_init(|| {
        let v: &'static mut Vec<u8> = Box::leak(Box::new(vec![0u8; REGION]));
        v.as_mut_ptr() as usize
    }) as *mut u8
}

#[no_mangle]
pub extern "C" fn clockwork_shm_base() -> *mut c_void {
    base_ptr().cast::<c_void>()
}

/// # Safety
/// Every out-pointer is valid for one write; this is the host-side symbol the
/// crate links against, and the crate calls it with its own locals.
#[no_mangle]
pub unsafe extern "C" fn clockwork_scope_geometry(
    start: *mut usize,
    header_size: *mut usize,
    slot_size: *mut usize,
    max_scopes: *mut usize,
    ring_frames: *mut u32,
    channels: *mut u32,
    track_start: *mut usize,
    track_slots: *mut usize,
) -> c_int {
    // SAFETY: valid for one write each, per the contract above.
    unsafe {
        *start = START;
        *header_size = HEADER_SIZE;
        *slot_size = SLOT_SIZE;
        *max_scopes = MAX_SCOPES;
        *ring_frames = RING_FRAMES;
        *channels = CHANNELS;
        *track_start = TRACK_START;
        *track_slots = TRACK_SLOTS;
    }
    1
}

// The crate's slot table is process-global, so the cases take a lock rather
// than racing each other through it.
fn lock() -> std::sync::MutexGuard<'static, Vec<u8>> {
    region().lock().unwrap_or_else(|e| e.into_inner())
}

/// A u32 at `offset` into the region, read as the crate reads it.
fn region_u32(offset: usize) -> u32 {
    // SAFETY: every offset passed is inside the leaked region, and 4-aligned:
    // the region starts page-aligned and the layout constants are multiples
    // of 16.
    unsafe { (*base_ptr().add(offset).cast::<AtomicU32>()).load(Ordering::Acquire) }
}

fn slot_state(index: usize) -> u32 {
    // `state` is the first u32 of the slot header (shm_scope_stream). One
    // index space: the guest's slots, then the track taps.
    if index < MAX_SCOPES {
        region_u32(START + HEADER_SIZE + SLOT_SIZE * index)
    } else {
        region_u32(TRACK_START + SLOT_SIZE * (index - MAX_SCOPES))
    }
}

/// The global header's active-slot counter.
fn active_count() -> u32 {
    region_u32(START + 4)
}

fn claim(index: usize, hnd: &mut ScopeBufferHnd) -> bool {
    // SAFETY: `hnd` is a live handle.
    unsafe {
        clockwork_scope_get(std::ptr::null_mut(), index as c_int, CHANNELS as c_int,
                      RING_FRAMES as c_int, hnd) != 0
    }
}

fn release(hnd: &mut ScopeBufferHnd) {
    // SAFETY: `hnd` is a live handle.
    unsafe { clockwork_scope_release(std::ptr::null_mut(), hnd) }
}

fn handle() -> ScopeBufferHnd {
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
    assert!(!claim(MAX_SCOPES + TRACK_SLOTS, &mut h), "a slot past both spans was claimed");
    assert!(h.internalData.is_null());

    let mut neg = handle();
    assert!(
        // SAFETY: `neg` is a live handle.
        unsafe {
            clockwork_scope_get(std::ptr::null_mut(), -1, CHANNELS as c_int,
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
    // the current region does not cover. Release used to treat "cannot
    // identify the slot" as "free it anyway" and store 0 through that
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
fn a_track_tap_is_a_scope_slot_numbered_after_the_guests_in_its_own_span() {
    let _g = lock();
    // The first index past the guest's slots lands in the track span, at
    // the track region's start, not one stride past the guest's last slot.
    let mut h = handle();
    assert!(claim(MAX_SCOPES, &mut h), "the first track tap was refused");
    assert_eq!(h.internalData as usize, base_ptr() as usize + TRACK_START);
    assert_eq!(slot_state(MAX_SCOPES), 1);
    assert_eq!(slot_state(MAX_SCOPES - 1), 0, "the guest's last slot was not touched");

    // Ownership holds across the span boundary: a later claimant of the
    // same tap supersedes, and the old handle's release is a no-op.
    let mut later = handle();
    assert!(claim(MAX_SCOPES, &mut later));
    release(&mut h);
    assert_eq!(slot_state(MAX_SCOPES), 1, "a superseded handle freed a track tap");
    release(&mut later);
    assert_eq!(slot_state(MAX_SCOPES), 0);

    // The last tap is the last slot there is.
    let mut last = handle();
    assert!(claim(MAX_SCOPES + TRACK_SLOTS - 1, &mut last));
    assert!(h.internalData.is_null());
    release(&mut last);
}
