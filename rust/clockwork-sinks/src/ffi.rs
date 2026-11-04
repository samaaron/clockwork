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

use clockwork_fence::no_unwind;
use std::os::raw::{c_char, c_int, c_uint};
use std::slice;

use std::sync::Arc;

use crate::profile::Class;
use crate::registry::Sinks;
use clockwork_log::ClockworkCore;
use crate::sink::Stats;
use clockwork_osc::Timetag;

// ── The table ───────────────────────────────────────────────────────────────
//
// Every call names the table its sink is in (`sinks`): a core's
// (clockwork_core_sinks), or one a host made for itself with
// clockwork_sinks_new. Null names none, and every call answers as for a sink
// that is not open. Each `# Safety` below adds to this one: `sinks` is null
// or a live table.

/// A table of sinks of the host's own — a test's — for clockwork_sinks_free.
/// `core`: whose lines the table's are; null for none's.
#[no_mangle]
pub extern "C" fn clockwork_sinks_new(core: *mut ClockworkCore) -> *const Sinks {
    no_unwind(core::ptr::null(), || {
        let _scope = clockwork_heap::router::Door::enter();   // what this makes is the router's
        Arc::into_raw(Sinks::new(core))
    })
}

/// Give back a table from clockwork_sinks_new; its sinks close when the
/// last holder lets it go.
///
/// # Safety
/// `sinks` came from clockwork_sinks_new and is given back once, or is null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sinks_free(sinks: *const Sinks) {
    no_unwind((), || {
        if sinks.is_null() { return; }
        let _scope = clockwork_heap::router::Door::enter();   // what this frees is the router's
        // SAFETY: from Arc::into_raw in clockwork_sinks_new, given back once.
        drop(unsafe { Arc::from_raw(sinks) });
    })
}

// ── Opening and closing ─────────────────────────────────────────────────────

/// # Safety
/// `sinks` is null or a live table;
/// `target` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_open(
    sinks: *const Sinks, kind: c_int, target: *const c_char, capacity: c_uint,
) -> c_uint {
    no_unwind(0, || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        let t = if target.is_null() {
            String::new()
        } else {
            // SAFETY: NUL-terminated, per the contract.
            unsafe { std::ffi::CStr::from_ptr(target) }.to_string_lossy().into_owned()
        };
        sinks.open(kind as u32, &t, capacity, false)
    })
}

/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_close(sinks: *const Sinks, sink: c_uint) {
    no_unwind((), || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return };
        sinks.close(sink);
    })
}

/// Install the host as the MIDI endpoint — or, with NULL, stop. See
/// `clockwork_event_sink.h` and [`crate::endpoint::HostEndpoint`]: while
/// installed, every MIDI sink opened from now on hands each message to the
/// emitter with its time and the emitter's context, and is driven direct.
/// Sinks already open keep the endpoint they have.
///
/// # Safety
/// `sinks` is null or a live table;
/// `emitter` is null or valid until the next call.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_set_host_emitter(sinks: *const Sinks, emitter: *const crate::endpoint::HostEmitter) {
    no_unwind((), || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return };
        sinks.set_host_emitter(emitter);
    })
}

// ── Size classes ────────────────────────────────────────────────────────────

/// Install the size classes every sink of `kind` opened from now on is made
/// of: `n` classes, `cell_bytes[i]` wide and `cells[i]` deep. Returns 1, or 0
/// for a shape that does not normalise (nothing, a repeated width) — the
/// previous shape, or the default, then stays.
///
/// # Safety
/// `sinks` is null or a live table;
/// `cell_bytes` and `cells` are valid for `n` reads, or `n` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_profile(
    sinks: *const Sinks, kind: c_int, n: c_uint, cell_bytes: *const c_uint, cells: *const c_uint,
) -> c_int {
    no_unwind(0, || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        if n == 0 || cell_bytes.is_null() || cells.is_null() { return 0; }
        // SAFETY: both valid for `n` reads, per the contract; nulls were refused.
        let (widths, depths) = unsafe {
            (slice::from_raw_parts(cell_bytes, n as usize), slice::from_raw_parts(cells, n as usize))
        };
        let classes = widths.iter().zip(depths)
            .map(|(&w, &d)| Class { cell_bytes: w, cells: d })
            .collect();
        sinks.install_shape(kind as u32, classes) as c_int
    })
}

/// The widest message `sink` can carry: the width of its widest class. 0 for
/// a handle that is not live.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_max_message_bytes(sinks: *const Sinks, sink: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        sinks.acquire(sink).map(|s| s.max_message_bytes()).unwrap_or(0)
    })
}

/// Payload bytes `sink` reserved at open, every class together. 0 for a
/// handle that is not live.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_bytes_reserved(sinks: *const Sinks, sink: c_uint) -> u64 {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        sinks.acquire(sink).map(|s| s.bytes_reserved()).unwrap_or(0)
    })
}

/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_is_open(sinks: *const Sinks, sink: c_uint) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        sinks.acquire(sink).is_some() as c_int
    })
}

/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_kind(sinks: *const Sinks, sink: c_uint) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        sinks.acquire(sink).map_or(0, |s| s.kind as c_int)
    })
}

/// The target given at open, or NULL.
///
/// The pointer is the sink's own `CString` and lives exactly as long as the
/// sink does. It is a diagnostics call and not one to hold onto: nothing here
/// can keep a target alive past the close that frees it.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_target(sinks: *const Sinks, sink: c_uint) -> *const c_char {
    no_unwind(core::ptr::null(), || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return core::ptr::null() };
        match sinks.acquire(sink) {
            Some(s) => s.target.as_ptr(),
            None => core::ptr::null(),
        }
    })
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
/// `sinks` is null or a live table;
/// `bytes` is valid for `len` reads, or NULL when `len` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_send(
    sinks: *const Sinks, sink: c_uint, bytes: *const u8, len: c_uint, when: Timetag,
) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        let Some(s) = sinks.acquire(sink) else { return 0 };
        if bytes.is_null() || len == 0 {
            // An empty message is not a message. Refused rather than counted as a
            // drop: nothing was lost.
            return 0;
        }
        // SAFETY: valid for `len` reads, per the contract; null was refused.
        let body = unsafe { slice::from_raw_parts(bytes, len as usize) };
        s.send(body, when) as c_int
    })
}

// ── Health ──────────────────────────────────────────────────────────────────

/// Fill `out` with the sink's counters. Returns non-zero if it did.
///
/// # Safety
/// `sinks` is null or a live table;
/// `out` points to a writable `ClockworkSinkStats`, or is NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_stats(sinks: *const Sinks, sink: c_uint, out: *mut Stats) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        if out.is_null() { return 0; }
        let Some(s) = sinks.acquire(sink) else { return 0 };
        // Written through as one struct: `Stats` is `#[repr(C)]` and is
        // `ClockworkSinkStats`, rather than a transcription of it that could drift.
        // SAFETY: writable, per the contract; null was refused.
        unsafe { *out = s.stats() };
        1
    })
}

/// # Safety
/// `sinks` is null or a live table;
/// `out` is valid for `cap` writes, or NULL when `cap` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_list(sinks: *const Sinks, out: *mut c_uint, cap: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        // SAFETY: the contract above is `sinks.list`'s.
        unsafe { sinks.list(out, cap) }
    })
}

/// Cancel everything one sink is holding; returns how many were cancelled.
/// A closed, stale or invented handle cancels nothing and answers 0.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_flush(sinks: *const Sinks, sink: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        sinks.flush(sink)
    })
}

/// Cancel everything EVERY open sink is holding; returns the total.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_flush_all(sinks: *const Sinks) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        sinks.flush_all()
    })
}

/// Close every open sink: shutdown wants one call rather than a list-and-loop,
/// and one test case must not leak slots into the next. Never the audio
/// thread's. What a sink still held is discarded, not sent.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_close_all(sinks: *const Sinks) {
    no_unwind((), || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return };
        sinks.close_all();
    })
}

/// Open a sink for the guest (DspHost::open_sink): as clockwork_sink_open,
/// and closed with the guest's instance (clockwork_sink_close_guests).
///
/// # Safety
/// `sinks` is null or a live table; `target` as clockwork_sink_open's.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_open_guest(
    sinks: *const Sinks, kind: c_int, target: *const c_char, capacity: c_uint,
) -> c_uint {
    no_unwind(0, || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(sinks) = (unsafe { sinks.as_ref() }) else { return 0 };
        let t = if target.is_null() {
            String::new()
        } else {
            // SAFETY: NUL-terminated, per the contract.
            unsafe { std::ffi::CStr::from_ptr(target) }.to_string_lossy().into_owned()
        };
        sinks.open(kind as u32, &t, capacity, true)
    })
}

/// Close every sink the guest opened: a guest's sinks last exactly as long as
/// its instance (dsp_api.h). Never the audio thread's.
///
/// # Safety
/// `sinks` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_sink_close_guests(sinks: *const Sinks) {
    no_unwind((), || {
        let _scope = clockwork_heap::router::Door::enter();   // what this frees is the router's
        // SAFETY: a live table, or null, per the contract.
        if let Some(sinks) = unsafe { sinks.as_ref() } { sinks.close_guests(); }
    })
}
