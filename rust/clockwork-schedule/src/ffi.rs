// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The C ABI of `rust/clockwork-schedule/cpp/clockwork_schedule.h`.
//!
//! One thin function per operation the C++ `Scheduler<>` template performs,
//! plus the four parser entry points. The template survives as a shim over
//! this, so clockwork's call sites did not move.
//!
//! # Panic freedom across the boundary
//!
//! Unwinding out of an `extern "C"` function is undefined behaviour, so none
//! of these may panic. Two things guarantee it:
//!
//! 1. Nothing below can panic. Every handle is null-checked here; every index
//!    inside [`crate::store`] comes from the free list or the heap and is
//!    in range by construction; the two size computations that could overflow
//!    a `u32` are performed in `u64`; there is no formatting, no `unwrap`, no
//!    slicing by a caller-supplied index that has not already been bounded.
//!    The only allocation is in `clockwork_sched_new`, whose failure is a null
//!    return rather than a panic (an allocator failure itself aborts, and has
//!    always aborted, in both languages).
//! 2. Belt as well as braces: since Rust 1.81 a `panic!` reaching an
//!    `extern "C"` frame aborts the process rather than unwinding into C++,
//!    so even a bug here is a crash with a message and not UB.

#![allow(unused_imports)]

use std::os::raw::{c_int, c_uint, c_void};

#[cfg(feature = "parse")]
use crate::parse;
#[cfg(feature = "store")]
use crate::store::Store;
#[cfg(feature = "store")]
use crate::tag;

// ── The store ───────────────────────────────────────────────────────────────
// Absent entirely without the `store` feature: not compiled and stubbed, not
// compiled at all, so a C++ site that was gated wrong is a link error rather
// than a silent no-op.
#[cfg(feature = "store")]
mod store_abi {
use super::*;

/// Opaque store handle. C sees `ClockworkSched*`.
pub struct ClockworkSched(Store);

// ── The store ───────────────────────────────────────────────────────────────

/// Build a store: `slot_count` events at most, a `data_pool_size`-byte payload
/// pool, and one `meta_size`-byte record per slot aligned to `meta_align`.
/// Returns null if the shape is impossible or the allocation failed.
#[no_mangle]
pub extern "C" fn clockwork_sched_new(
    slot_count: c_uint,
    data_pool_size: c_uint,
    meta_size: c_uint,
    meta_align: c_uint,
) -> *mut ClockworkSched {
    match Store::new(slot_count, data_pool_size, meta_size, meta_align) {
        Some(s) => Box::into_raw(Box::new(ClockworkSched(s))),
        None => core::ptr::null_mut(),
    }
}

/// Bytes a caller must supply to [`clockwork_sched_new_in_place`] for this shape,
/// or 0 if the shape is invalid.
#[no_mangle]
pub extern "C" fn clockwork_sched_bytes_needed(
    slot_count: c_uint,
    data_pool_size: c_uint,
    meta_size: c_uint,
    meta_align: c_uint,
) -> usize {
    // The handle itself lives in the caller's buffer too.
    match Store::bytes_needed(slot_count, data_pool_size, meta_size, meta_align) {
        Some(n) => n + core::mem::size_of::<ClockworkSched>() + core::mem::align_of::<ClockworkSched>(),
        None => 0,
    }
}

/// Build a scheduler inside memory the caller owns. Allocates nothing, so it
/// cannot fail for want of a heap — which is the failure that left the web
/// build with a scheduler that silently accepted nothing.
///
/// # Safety
/// `mem` must point at `len` writable bytes that outlive the scheduler.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_new_in_place(
    mem: *mut u8,
    len: usize,
    slot_count: c_uint,
    data_pool_size: c_uint,
    meta_size: c_uint,
    meta_align: c_uint,
) -> *mut ClockworkSched {
    let needed = clockwork_sched_bytes_needed(slot_count, data_pool_size, meta_size, meta_align);
    if mem.is_null() || needed == 0 || len < needed {
        return core::ptr::null_mut();
    }
    // Put the handle at the front, aligned, and give the store the rest.
    let align = core::mem::align_of::<ClockworkSched>();
    let head = ((mem as usize + align - 1) & !(align - 1)) as *mut ClockworkSched;
    let used = head as usize - mem as usize + core::mem::size_of::<ClockworkSched>();
    // SAFETY: `used <= needed <= len`, so the rest of the buffer starts
    // inside it and is `len - used` bytes long, per the contract.
    let rest = unsafe { mem.add(used) };
    // SAFETY: `rest` is `len - used` writable bytes that outlive the store,
    // per the contract.
    let store = match unsafe { Store::new_in(rest, len - used, slot_count, data_pool_size, meta_size, meta_align) } {
        Some(s) => s,
        None => return core::ptr::null_mut(),
    };
    // SAFETY: `head` is aligned, inside the buffer, and the store was given
    // only the bytes after it.
    unsafe { core::ptr::write(head, ClockworkSched(store)) };
    head
}

/// # Safety
/// `h` is null or a handle from [`clockwork_sched_new`] not yet freed.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_free(h: *mut ClockworkSched) {
    if !h.is_null() {
        // SAFETY: the box `clockwork_sched_new` leaked, freed once, here.
        unsafe { drop(Box::from_raw(h)) };
    }
}

/// Store `size` bytes of `data` to fire at `when` under `tag`.
///
/// Returns the caller's metadata slot for this event — write the record there
/// — or null if the slot pool or the data pool is full, with nothing changed.
/// The pointer stays valid until the event's slot is released.
///
/// # Safety
/// `h` is a live handle or null; `data` is readable for `size` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_add(
    h: *mut ClockworkSched,
    when: i64,
    tag: c_uint,
    data: *const u8,
    size: c_uint,
) -> *mut c_void {
    // SAFETY: a live handle or null, per the contract.
    let Some(s) = (unsafe { h.as_mut() }) else { return core::ptr::null_mut() };
    let bytes: &[u8] = if data.is_null() || size == 0 {
        &[]
    } else {
        // SAFETY: readable for `size` bytes per the contract.
        unsafe { core::slice::from_raw_parts(data, size as usize) }
    };
    match s.0.add(when, tag, bytes) {
        Some(meta) => meta.as_mut_ptr().cast::<c_void>(),
        None => core::ptr::null_mut(),
    }
}

/// Timetag of the earliest live event, or `INT64_MAX` if none.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_next_time(h: *const ClockworkSched) -> i64 {
    // SAFETY: a live handle or null, per the contract.
    match unsafe { h.as_ref() } {
        Some(s) => s.0.next_time(),
        None => i64::MAX,
    }
}

/// A due event, as C sees it. Mirrors `ClockworkSchedEvent` in the header.
#[repr(C)]
pub struct ClockworkSchedEvent {
    pub when: i64,
    pub tag: c_uint,
    pub size: c_uint,
    pub meta: *const c_void,
    pub data: *const u8,
    pub slot: c_int,
}

/// Pop the earliest event if it is due at or before `now`, filling `out`.
/// Returns 1 when one was popped, 0 when nothing is due. `out` is untouched
/// on 0.
///
/// # Safety
/// `h` is a live handle or null; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_pop_due(
    h: *mut ClockworkSched,
    now: i64,
    out: *mut ClockworkSchedEvent,
) -> c_int {
    // SAFETY: a live handle or null, per the contract.
    let Some(s) = (unsafe { h.as_mut() }) else { return 0 };
    if out.is_null() {
        return 0;
    }
    match s.0.pop_due(now) {
        Some(e) => {
            // The slices become pointers here: the C contract is that they
            // stay valid until clockwork_sched_release for this slot.
            // SAFETY: `out` is writable, per the contract.
            unsafe { *out = ClockworkSchedEvent {
                when: e.when,
                tag: e.tag,
                size: e.data.len() as c_uint,
                meta: e.meta.as_ptr().cast::<c_void>(),
                data: e.data.as_ptr(),
                slot: e.slot,
            } };
            1
        }
        None => 0,
    }
}

/// Return a popped event's slot. Negative, out-of-range and already-free
/// slots are no-ops.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_release(h: *mut ClockworkSched, slot: c_int) {
    // SAFETY: a live handle or null, per the contract.
    if let Some(s) = unsafe { h.as_mut() } {
        s.0.release(slot);
    }
}

/// Cancel every live event carrying `tag`; tag 0 is the wildcard.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_flush(h: *mut ClockworkSched, tag: c_uint) {
    // SAFETY: a live handle or null, per the contract.
    if let Some(s) = unsafe { h.as_mut() } {
        s.0.flush(tag);
    }
}

/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_clear(h: *mut ClockworkSched) {
    // SAFETY: a live handle or null, per the contract.
    if let Some(s) = unsafe { h.as_mut() } {
        s.0.clear();
    }
}

/// Live event count.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_size(h: *const ClockworkSched) -> c_int {
    // SAFETY: a live handle or null, per the contract.
    unsafe { h.as_ref() }.map_or(0, |s| s.0.size())
}

/// 1 when the SLOT pool is full. The data pool can be exhausted while slots
/// remain free, and this does not report that: an `add` refusal is the only
/// complete signal.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_full(h: *const ClockworkSched) -> c_int {
    // SAFETY: a live handle or null, per the contract.
    unsafe { h.as_ref() }.map_or(1, |s| s.0.full() as c_int)
}

/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_data_used(h: *const ClockworkSched) -> c_uint {
    // SAFETY: a live handle or null, per the contract.
    unsafe { h.as_ref() }.map_or(0, |s| s.0.data_used())
}

/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_data_capacity(h: *const ClockworkSched) -> c_uint {
    // SAFETY: a live handle or null, per the contract.
    unsafe { h.as_ref() }.map_or(0, |s| s.0.data_capacity())
}

/// Ask the owning thread to clear at its next safe point. The one call here
/// another thread may make.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_request_clear(h: *const ClockworkSched) {
    // SAFETY: a live handle or null, per the contract.
    if let Some(s) = unsafe { h.as_ref() } {
        s.0.request_clear();
    }
}

/// Perform a requested clear, if there is one. Returns 1 if it happened.
///
/// # Safety
/// `h` is a live handle or null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_drain_pending_clear(h: *mut ClockworkSched) -> c_int {
    // SAFETY: a live handle or null, per the contract.
    unsafe { h.as_mut() }.map_or(0, |s| s.0.drain_pending_clear() as c_int)
}

}

#[cfg(feature = "store")]
pub use store_abi::*;

// ── Tags ────────────────────────────────────────────────────────────────────

/// FNV-1a over exactly `n` bytes, clamped away from the 0 wildcard.
///
/// `src/scheduler/Scheduler.h` keeps its own `constexpr` twin because the C++
/// tests assert it in `static_assert`; see `crate::tag` for why that is by
/// design. This entry point is for callers with no such constraint.
///
/// # Safety
/// `s` is readable for `n` bytes, or null when `n` is 0.
#[cfg(feature = "store")]
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_tag_hash(s: *const u8, n: usize) -> c_uint {
    if s.is_null() || n == 0 {
        return tag::tag_hash(&[]);
    }
    // SAFETY: readable for `n` bytes, per the contract.
    tag::tag_hash(unsafe { core::slice::from_raw_parts(s, n) })
}

// ── The wire forms ──────────────────────────────────────────────────────────
// Present without the store: a timestamped bundle is the DSP's, and a build
// with no store still has to read its timetag to forward it honestly.

/// Mirrors `ClockworkSchedulePacket` in the header. `ok` is 0/1 rather than a C++
/// `bool` so the ABI does not depend on how a given C++ compiler lays one out.
#[cfg(feature = "parse")]
#[repr(C)]
pub struct ClockworkSchedulePacket {
    pub ok: c_int,
    pub when: i64,
    pub blob: *const u8,
    pub blob_len: c_uint,
}

/// NTP seconds into an OSC 32.32 timetag.
#[cfg(feature = "parse")]
#[no_mangle]
pub extern "C" fn clockwork_sched_ntp_to_timetag(ntp: f64) -> i64 {
    parse::ntp_to_timetag(ntp)
}

/// # Safety
/// `data` is readable for `size` bytes, or null.
#[cfg(feature = "parse")]
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_is_bundle(data: *const u8, size: c_uint) -> c_int {
    if data.is_null() { return 0; }
    // SAFETY: readable for `size` bytes per the contract.
    parse::is_bundle(unsafe { core::slice::from_raw_parts(data, size as usize) }) as c_int
}

/// # Safety
/// `bundle` is readable for at least 16 bytes.
#[cfg(feature = "parse")]
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_bundle_timetag(bundle: *const u8) -> u64 {
    if bundle.is_null() {
        return 0;
    }
    // SAFETY: readable for at least 16 bytes per the contract.
    parse::bundle_timetag(unsafe { core::slice::from_raw_parts(bundle, 16) })
}

/// Parse `"/clockwork/schedule <timetag> <blob>"`. `blob` borrows `msg`.
///
/// # Safety
/// `msg` is readable for `size` bytes, or null.
#[cfg(feature = "parse")]
#[no_mangle]
pub unsafe extern "C" fn clockwork_sched_parse(msg: *const u8, size: c_uint) -> ClockworkSchedulePacket {
    if msg.is_null() {
        return ClockworkSchedulePacket { ok: 0, when: 0, blob: core::ptr::null(), blob_len: 0 };
    }
    // SAFETY: readable for `size` bytes per the contract.
    let bytes = unsafe { core::slice::from_raw_parts(msg, size as usize) };
    let p = parse::parse(bytes);
    // The blob's offset becomes a pointer into the caller's message here.
    let blob = if p.ok { bytes[p.blob_off..].as_ptr() } else { core::ptr::null() };
    ClockworkSchedulePacket { ok: p.ok as c_int, when: p.when, blob, blob_len: p.blob_len }
}
