// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The C ABI of `src/clockwork_event_sink.h`, one function at a time.
//!
//! Everything here is a thin skin over `registry::acquire` plus one call on
//! the sink behind it. The only judgement calls it makes are the ones the
//! header names: drop when full rather than block, never fault on a handle
//! that is stale or closed or nonsense, and never let a send touch anything
//! that could stall the audio thread.
//!
//! # A closed or stale handle is refused, not diagnosed
//!
//! `clockwork_sink_send` on a handle whose sink has closed answers 0 and counts
//! nothing — there is no sink to count it against, and a global "sends to
//! nowhere" number would say nothing about any particular destination. Every
//! query answers the empty thing: not open, kind 0, target NULL, stats
//! untouched. Same rule as `clockwork_ports.h`'s, for the same reason: the audio
//! thread is not the place to discover a lifetime bug, and a fault there is a
//! silence in the speakers.

use std::os::raw::{c_char, c_int, c_uint};
use std::slice;

use crate::profile::{self, Class};
use crate::registry;
use crate::sink::Stats;

// ── Opening and closing ─────────────────────────────────────────────────────

/// # Safety
/// `target` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_open(
    kind: c_int, target: *const c_char, capacity: c_uint,
) -> c_uint {
    let t = if target.is_null() {
        String::new()
    } else {
        // SAFETY: NUL-terminated, per the contract.
        unsafe { std::ffi::CStr::from_ptr(target) }.to_string_lossy().into_owned()
    };
    registry::open(kind as u32, &t, capacity)
}

#[no_mangle]
pub extern "C" fn clockwork_sink_close(sink: c_uint) {
    registry::close(sink);
}

/// Install the host as the MIDI endpoint — or, with NULL, stop. See
/// `clockwork_event_sink.h` and [`crate::endpoint::HostEndpoint`]: while
/// installed, every MIDI sink opened from now on hands each message to
/// `emit` with its time, and is driven direct. Sinks already open keep the
/// endpoint they have.
#[no_mangle]
pub extern "C" fn clockwork_sink_set_host_emit(emit: Option<crate::endpoint::HostEmit>) {
    crate::endpoint::set_host_emit(emit);
}

// ── Size classes ────────────────────────────────────────────────────────────

/// Install the size classes every sink of `kind` opened from now on is made
/// of: `n` classes, `cell_bytes[i]` wide and `cells[i]` deep. Returns 1, or 0
/// for a shape that does not normalise (nothing, a repeated width) — the
/// previous shape, or the default, then stays.
///
/// # Safety
/// `cell_bytes` and `cells` are valid for `n` reads, or `n` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_profile(
    kind: c_int, n: c_uint, cell_bytes: *const c_uint, cells: *const c_uint,
) -> c_int {
    if n == 0 || cell_bytes.is_null() || cells.is_null() { return 0; }
    // SAFETY: both valid for `n` reads, per the contract; nulls were refused.
    let (widths, depths) = unsafe {
        (slice::from_raw_parts(cell_bytes, n as usize), slice::from_raw_parts(cells, n as usize))
    };
    let classes = widths.iter().zip(depths)
        .map(|(&w, &d)| Class { cell_bytes: w, cells: d })
        .collect();
    profile::install(kind as u32, classes) as c_int
}

/// The widest message `sink` can carry: the width of its widest class. 0 for
/// a handle that is not live.
#[no_mangle]
pub extern "C" fn clockwork_sink_max_message_bytes(sink: c_uint) -> c_uint {
    registry::acquire(sink).map(|s| s.max_message_bytes()).unwrap_or(0)
}

/// Payload bytes `sink` reserved at open, every class together. 0 for a
/// handle that is not live.
#[no_mangle]
pub extern "C" fn clockwork_sink_bytes_reserved(sink: c_uint) -> u64 {
    registry::acquire(sink).map(|s| s.bytes_reserved()).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn clockwork_sink_is_open(sink: c_uint) -> c_int {
    registry::acquire(sink).is_some() as c_int
}

#[no_mangle]
pub extern "C" fn clockwork_sink_kind(sink: c_uint) -> c_int {
    registry::acquire(sink).map_or(0, |s| s.kind as c_int)
}

/// The target given at open, or NULL.
///
/// The pointer is the sink's own `CString` and lives exactly as long as the
/// sink does. It is a diagnostics call and not one to hold onto: nothing here
/// can keep a target alive past the close that frees it.
#[no_mangle]
pub extern "C" fn clockwork_sink_target(sink: c_uint) -> *const c_char {
    match registry::acquire(sink) {
        Some(s) => s.target.as_ptr(),
        None => core::ptr::null(),
    }
}

// ── Sending ─────────────────────────────────────────────────────────────────

/// Send one message, to be delivered at `when`.
///
/// THE AUDIO THREAD'S CALL. What it does: one compare-exchange to enter the
/// slot, one compare-exchange to claim a queue position, and a `memcpy` into
/// memory taken at open. No allocation, no lock, no wait, and no branch on
/// what kind of endpoint is on the other end.
///
/// # Safety
/// `bytes` is valid for `len` reads, or NULL when `len` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_send(
    sink: c_uint, bytes: *const u8, len: c_uint, when: i64,
) -> c_int {
    let Some(s) = registry::acquire(sink) else { return 0 };
    if bytes.is_null() || len == 0 {
        // An empty message is not a message. Refused rather than counted as a
        // drop: nothing was lost.
        return 0;
    }
    // SAFETY: valid for `len` reads, per the contract; null was refused.
    let body = unsafe { slice::from_raw_parts(bytes, len as usize) };
    s.send(body, when) as c_int
}

// ── Health ──────────────────────────────────────────────────────────────────

/// Fill `out` with the sink's counters. Returns non-zero if it did.
///
/// # Safety
/// `out` points to a writable `ClockworkSinkStats`, or is NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_stats(sink: c_uint, out: *mut Stats) -> c_int {
    if out.is_null() { return 0; }
    let Some(s) = registry::acquire(sink) else { return 0 };
    // Written through as one struct: `Stats` is `#[repr(C)]` and is
    // `ClockworkSinkStats`, rather than a transcription of it that could drift.
    // SAFETY: writable, per the contract; null was refused.
    unsafe { *out = s.stats() };
    1
}

/// # Safety
/// `out` is valid for `cap` writes, or NULL when `cap` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_list(out: *mut c_uint, cap: c_uint) -> c_uint {
    // SAFETY: the contract above is `registry::list`'s.
    unsafe { registry::list(out, cap) }
}

/// Close every open sink.
///
/// NOT in the original `src/clockwork_event_sink.h`, and added for the same two
/// reasons `clockwork_port_close_all` was: shutdown wants one call rather than a
/// list-and-loop, and one test case must not be able to leak slots into the
/// next. Never the audio thread's.
/// Cancel everything one sink is holding; returns how many were cancelled.
/// A closed, stale or invented handle cancels nothing and answers 0.
#[no_mangle]
pub extern "C" fn clockwork_sink_flush(sink: c_uint) -> c_uint {
    crate::registry::flush(sink)
}

/// Cancel everything EVERY open sink is holding; returns the total.
#[no_mangle]
pub extern "C" fn clockwork_sink_flush_all() -> c_uint {
    crate::registry::flush_all()
}

#[no_mangle]
pub extern "C" fn clockwork_sink_close_all() {
    registry::close_all();
}
