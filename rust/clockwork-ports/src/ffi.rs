// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The C ABI of `src/clockwork_ports.h`, one function at a time.
//!
//! Everything here is a thin skin over `registry::acquire` plus a ring
//! operation. The only judgement calls it makes are the ones the header names:
//! silence on underrun, drop on overrun, count both, refuse nothing over a
//! channel-count mismatch, and never fault on a handle that is stale, closed
//! or nonsense.
//!
//! # Direction is enforced, and it is silent about it
//!
//! `clockwork_port_read` on a SINK, or `clockwork_port_write` on a SOURCE, is a category
//! error: a sink's ring is filled by the audio thread and drained by the
//! endpoint, so reading one would hand the audio thread its own output back.
//! Both answer 0 (a read also zeroes its block, so the caller may still use
//! it) and neither touches a counter — an underrun means "the endpoint was
//! late", and calling the wrong function is not that. The same goes for
//! `clockwork_port_produce` on a sink and `clockwork_port_consume` on a source.

use core::sync::atomic::Ordering;
use core::slice;
use std::os::raw::{c_char, c_int, c_uint};

use crate::registry;

// ── Opening and closing ─────────────────────────────────────────────────────

/// # Safety
/// `name` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_open(
    name: *const c_char, dir: c_int, channels: c_uint, capacity_frames: c_uint,
) -> c_uint {
    let n = if name.is_null() {
        String::new()
    } else {
        // SAFETY: NUL-terminated, per the contract.
        unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy().into_owned()
    };
    registry::open(&n, dir as u32, channels, capacity_frames)
}

/// Open a port over caller-owned memory (`clockwork_ports.h`, "shared storage").
///
/// # Safety
/// `name` as for `clockwork_port_open`; `storage` valid for `storage_bytes` bytes,
/// 64-byte aligned, and mapped for as long as the port is open.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_open_shared(
    name: *const c_char, dir: c_int, channels: c_uint, capacity_frames: c_uint,
    storage: *mut u8, storage_bytes: usize, reset: c_int,
) -> c_uint {
    let n = if name.is_null() {
        String::new()
    } else {
        // SAFETY: NUL-terminated, per the contract.
        unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy().into_owned()
    };
    // SAFETY: the storage contract above is `registry::open_shared`'s.
    unsafe { registry::open_shared(&n, dir as u32, channels, capacity_frames, storage, storage_bytes, reset != 0) }
}

/// Bytes `clockwork_port_open_shared` needs for this shape, header included. Zero
/// for a shape a port cannot be, so a caller sizing a segment finds out here
/// rather than at open.
#[no_mangle]
pub extern "C" fn clockwork_port_shared_bytes(channels: c_uint, capacity_frames: c_uint) -> usize {
    if channels == 0 || channels > crate::ring::MAX_CHANNELS { return 0; }
    if capacity_frames != crate::ring::round_capacity(capacity_frames) { return 0; }
    crate::ring::storage_bytes(capacity_frames, channels)
}

/// Install (NULL `f` removes) the transfer signal: called on the audio thread
/// after each `clockwork_port_write` on a sink or `clockwork_port_read` on a source, with
/// `ctx`. Returns 0 if `port` is not open.
#[no_mangle]
pub extern "C" fn clockwork_port_set_signal(
    port: c_uint, f: Option<registry::Signal>, ctx: *mut core::ffi::c_void,
) -> c_int {
    registry::set_signal(port, f, ctx) as c_int
}

#[no_mangle]
pub extern "C" fn clockwork_port_close(port: c_uint) {
    registry::close(port);
}

#[no_mangle]
pub extern "C" fn clockwork_port_channels(port: c_uint) -> c_uint {
    registry::acquire(port).map_or(0, |h| h.get().channels)
}

#[no_mangle]
pub extern "C" fn clockwork_port_is_open(port: c_uint) -> c_int {
    registry::acquire(port).is_some() as c_int
}

/// The direction the port was opened with, or 0 if it is not open.
///
/// NOT in the original `src/clockwork_ports.h`, and added because every consumer
/// needs it: `clockwork_port_list` hands back both directions mixed, and a caller
/// that means to read sources cannot tell which of them it may read. Without
/// this clockwork would have to keep its own shadow record of a fact the
/// substrate already knows, which is the kind of duplication ports exist to
/// end.
#[no_mangle]
pub extern "C" fn clockwork_port_direction(port: c_uint) -> c_int {
    registry::acquire(port).map_or(0, |h| h.get().direction as c_int)
}

// ── The audio thread ────────────────────────────────────────────────────────
//
// Pointers become slices here, once, and registry.rs does the rest. A null
// channel pointer is `None`; more channels than a ring can have are ignored
// rather than indexed.

const MAX_CH: usize = crate::ring::MAX_CHANNELS as usize;

/// # Safety
/// `out` points to `channels` pointers, each valid for `frames` writes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_read(
    port: c_uint, out: *const *mut f32, channels: c_uint, frames: c_uint,
) -> c_uint {
    let frames = frames as usize;
    let mut dst: [Option<&mut [f32]>; MAX_CH] = [const { None }; MAX_CH];
    if !out.is_null() {
        for (c, d) in dst.iter_mut().enumerate().take((channels as usize).min(MAX_CH)) {
            // SAFETY: `out` has `channels` entries per the contract.
            let p = unsafe { *out.add(c) };
            if !p.is_null() {
                // SAFETY: each non-null entry is valid for `frames` writes.
                *d = Some(unsafe { slice::from_raw_parts_mut(p, frames) });
            }
        }
    }
    registry::read(port, &mut dst, frames)
}

/// # Safety
/// `input` points to `channels` pointers, each valid for `frames` reads.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_write(
    port: c_uint, input: *const *const f32, channels: c_uint, frames: c_uint,
) -> c_uint {
    let frames = frames as usize;
    let mut src: [Option<&[f32]>; MAX_CH] = [None; MAX_CH];
    if !input.is_null() {
        for (c, s) in src.iter_mut().enumerate().take((channels as usize).min(MAX_CH)) {
            // SAFETY: `input` has `channels` entries per the contract.
            let p = unsafe { *input.add(c) };
            if !p.is_null() {
                // SAFETY: each non-null entry is valid for `frames` reads.
                *s = Some(unsafe { slice::from_raw_parts(p, frames) });
            }
        }
    }
    registry::write(port, &src, frames)
}

// ── The endpoint's side ─────────────────────────────────────────────────────

/// # Safety
/// `interleaved` is valid for `frames * clockwork_port_channels(port)` reads.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_produce(
    port: c_uint, interleaved: *const f32, frames: c_uint,
) -> c_uint {
    if interleaved.is_null() { return 0; }
    let ch = registry::channels(port) as usize;
    // SAFETY: valid for `frames * channels` reads per the contract.
    let src = unsafe { slice::from_raw_parts(interleaved, frames as usize * ch) };
    registry::produce(port, src)
}

/// # Safety
/// `interleaved` is valid for `frames * clockwork_port_channels(port)` writes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_consume(
    port: c_uint, interleaved: *mut f32, frames: c_uint,
) -> c_uint {
    if interleaved.is_null() { return 0; }
    let ch = registry::channels(port) as usize;
    // SAFETY: valid for `frames * channels` writes per the contract.
    let dst = unsafe { slice::from_raw_parts_mut(interleaved, frames as usize * ch) };
    registry::consume(port, dst)
}

#[no_mangle]
pub extern "C" fn clockwork_port_writable(port: c_uint) -> c_uint {
    registry::acquire(port).map_or(0, |h| h.get().ring.writable() as u32)
}

#[no_mangle]
pub extern "C" fn clockwork_port_readable(port: c_uint) -> c_uint {
    registry::acquire(port).map_or(0, |h| h.get().ring.readable() as u32)
}

// ── Health ──────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn clockwork_port_underruns(port: c_uint) -> u64 {
    registry::acquire(port).map_or(0, |h| h.get().underruns.load(Ordering::Relaxed))
}

#[no_mangle]
pub extern "C" fn clockwork_port_overruns(port: c_uint) -> u64 {
    registry::acquire(port).map_or(0, |h| h.get().overruns.load(Ordering::Relaxed))
}

/// # Safety
/// `out` is valid for `cap` writes, or NULL when `cap` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_list(out: *mut c_uint, cap: c_uint) -> c_uint {
    // SAFETY: the contract above is `registry::list`'s.
    unsafe { registry::list(out, cap) }
}

/// The name given at open, or NULL.
///
/// The pointer is the port's own `CString` and lives exactly as long as the
/// port does, which is what the header promises ("valid until the port
/// closes"). It is therefore a diagnostics call and not one to hold onto:
/// nothing here can keep a name alive past the close that frees it.
#[no_mangle]
pub extern "C" fn clockwork_port_name(port: c_uint) -> *const c_char {
    match registry::acquire(port) {
        Some(h) => h.get().name.as_ptr(),
        None => core::ptr::null(),
    }
}

/// Close every open port. Not in `clockwork_ports.h`; a test and shutdown helper, so
/// one case cannot leak slots into the next.
#[no_mangle]
pub extern "C" fn clockwork_port_close_all() {
    registry::close_all();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::registry::{DIR_SINK, DIR_SOURCE};
    use portable_atomic::AtomicU64;

    /// The signal fires AFTER the transfer, on the calling thread, so the
    /// receiver can act on the frames that just moved: here it counts what
    /// the ring holds at the moment it fires, which must include the block.
    #[test]
    fn the_signal_fires_after_the_frames_have_moved() {
        let _t = registry::table_lock();
        static SEEN: AtomicU64 = AtomicU64::new(0);
        unsafe extern "C" fn on_write(port: u32, _ctx: *mut core::ffi::c_void) {
            SEEN.store(clockwork_port_readable(port) as u64, Ordering::Relaxed);
        }
        // SAFETY: a NUL-terminated literal.
        let h = unsafe { clockwork_port_open(c"sig".as_ptr(), DIR_SINK as c_int, 1, 64) };
        assert_eq!(clockwork_port_set_signal(h, Some(on_write), core::ptr::null_mut()), 1);
        let block = [0.5f32; 32];
        let ptrs = [block.as_ptr()];
        // SAFETY: one channel pointer to 32 live frames.
        unsafe { assert_eq!(clockwork_port_write(h, ptrs.as_ptr(), 1, 32), 32) };
        assert_eq!(SEEN.load(Ordering::Relaxed), 32);
        // A read on a sink is a category error and moves nothing, so it
        // does not signal either.
        let mut out = [0f32; 32];
        let optrs = [out.as_mut_ptr()];
        SEEN.store(99, Ordering::Relaxed);
        // SAFETY: one channel pointer to 32 writable frames.
        unsafe { assert_eq!(clockwork_port_read(h, optrs.as_ptr(), 1, 32), 0) };
        assert_eq!(SEEN.load(Ordering::Relaxed), 99);
        clockwork_port_close(h);
    }

    /// Two ports over one block of memory, a sink on one "side" and a source
    /// on the other, are the whole transport of the plugin bridge in
    /// miniature: what the audio thread writes into the sink, the far side's
    /// `clockwork_port_consume` reads, and the geometry is what
    /// `clockwork_port_shared_bytes` said it would be.
    #[test]
    fn a_shared_sink_and_a_shared_source_meet_in_the_same_memory() {
        let _t = registry::table_lock();
        let bytes = clockwork_port_shared_bytes(2, 256);
        assert_eq!(bytes, 128 + 256 * 2 * 4);
        assert_eq!(clockwork_port_shared_bytes(0, 256), 0);
        assert_eq!(clockwork_port_shared_bytes(2, 200), 0, "not a power of two");
        #[repr(align(64))]
        struct Aligned([u8; 128 + 256 * 2 * 4]);
        let mut mem = Aligned([0u8; 128 + 256 * 2 * 4]);
        let base = mem.0.as_mut_ptr();
        let name = c"shared".as_ptr();
        // SAFETY: `mem` is 64-aligned, `bytes` long, and outlives both ports.
        let sink = unsafe { clockwork_port_open_shared(name, DIR_SINK as c_int, 2, 256, base, bytes, 1) };
        // SAFETY: as above.
        let src = unsafe { clockwork_port_open_shared(name, DIR_SOURCE as c_int, 2, 256, base, bytes, 0) };
        assert_ne!(sink, 0);
        assert_ne!(src, 0);
        let l = [1.0f32; 64];
        let r = [2.0f32; 64];
        let planar = [l.as_ptr(), r.as_ptr()];
        // SAFETY: two channel pointers to 64 live frames each.
        unsafe { assert_eq!(clockwork_port_write(sink, planar.as_ptr(), 2, 64), 64) };
        // The far side sees the block through the source's counters.
        assert_eq!(clockwork_port_readable(src), 64);
        let mut out = [0f32; 64 * 2];
        // SAFETY: `out` holds 64 frames of 2 channels.
        unsafe { assert_eq!(clockwork_port_consume(sink, out.as_mut_ptr(), 64), 64) };
        assert_eq!(out[0], 1.0);
        assert_eq!(out[1], 2.0);
        assert_eq!(clockwork_port_readable(src), 0);
        clockwork_port_close(sink);
        clockwork_port_close(src);
    }
}
