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

use clockwork_fence::no_unwind;
use core::sync::atomic::Ordering;
use core::slice;
use std::os::raw::{c_char, c_int, c_uint};

use std::sync::Arc;

use crate::registry::{self, Ports};

// ── The table ───────────────────────────────────────────────────────────────
//
// Every call names the table its port is in (`ports`): a core's
// (clockwork_core_ports), or one a host made for itself with
// clockwork_ports_new. Null names none, and every call answers as for a port
// that is not open. Each `# Safety` below adds to this one: `ports` is null
// or a live table.

/// A table of ports of the host's own — a plugin bridge's, a test's — for
/// clockwork_ports_free. Counted: a disk endpoint opened on one of its ports
/// keeps it for as long as it runs.
#[no_mangle]
pub extern "C" fn clockwork_ports_new() -> *const Ports {
    no_unwind(core::ptr::null(), || {
        Arc::into_raw(Arc::new(Ports::new()))
    })
}

/// Give back a table from clockwork_ports_new; its ports close when the last
/// holder lets it go.
///
/// # Safety
/// `ports` is null or a live table;
/// `ports` came from clockwork_ports_new and is given back once, or is null.
#[no_mangle]
pub unsafe extern "C" fn clockwork_ports_free(ports: *const Ports) {
    no_unwind((), || {
        if ports.is_null() { return; }
        // SAFETY: from Arc::into_raw in clockwork_ports_new, given back once.
        drop(unsafe { Arc::from_raw(ports) });
    })
}

// ── Opening and closing ─────────────────────────────────────────────────────

/// # Safety
/// `ports` is null or a live table;
/// `name` is a NUL-terminated C string or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_open(
    ports: *const Ports, name: *const c_char, dir: c_int, channels: c_uint, capacity_frames: c_uint,
) -> c_uint {
    no_unwind(0, || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        let n = if name.is_null() {
            String::new()
        } else {
            // SAFETY: NUL-terminated, per the contract.
            unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy().into_owned()
        };
        ports.open(&n, dir as u32, channels, capacity_frames)
    })
}

/// Open a port over caller-owned memory (`clockwork_ports.h`, "shared storage").
///
/// # Safety
/// `ports` is null or a live table;
/// `name` as for `clockwork_port_open`; `storage` valid for `storage_bytes` bytes,
/// 64-byte aligned, and mapped for as long as the port is open.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_open_shared(
    ports: *const Ports, name: *const c_char, dir: c_int, channels: c_uint, capacity_frames: c_uint,
    storage: *mut u8, storage_bytes: usize, reset: c_int,
) -> c_uint {
    no_unwind(0, || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        let n = if name.is_null() {
            String::new()
        } else {
            // SAFETY: NUL-terminated, per the contract.
            unsafe { std::ffi::CStr::from_ptr(name) }.to_string_lossy().into_owned()
        };
        let storage = registry::SharedStorage { base: storage, bytes: storage_bytes, reset: reset != 0 };
        // SAFETY: the storage contract above is `SharedStorage`'s.
        unsafe { ports.open_shared(&n, dir as u32, channels, capacity_frames, storage) }
    })
}

/// Bytes `clockwork_port_open_shared` needs for this shape, header included. Zero
/// for a shape a port cannot be, so a caller sizing a segment finds out here
/// rather than at open.
#[no_mangle]
pub extern "C" fn clockwork_port_shared_bytes(channels: c_uint, capacity_frames: c_uint) -> usize {
    no_unwind(0, || {
        if channels == 0 || channels > crate::ring::MAX_CHANNELS { return 0; }
        if capacity_frames != crate::ring::round_capacity(capacity_frames) { return 0; }
        crate::ring::storage_bytes(capacity_frames, channels)
    })
}

/// Install (NULL `f` removes) the transfer signal: called on the audio thread
/// after each `clockwork_port_write` on a sink or `clockwork_port_read` on a source, with
/// `ctx`. Returns 0 if `port` is not open.
///
/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_set_signal(
    ports: *const Ports, port: c_uint, f: Option<registry::Signal>, ctx: *mut core::ffi::c_void,
) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.set_signal(port, f, ctx) as c_int
    })
}

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_close(ports: *const Ports, port: c_uint) {
    no_unwind((), || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return };
        ports.close(port);
    })
}

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_channels(ports: *const Ports, port: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().channels)
    })
}

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_is_open(ports: *const Ports, port: c_uint) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).is_some() as c_int
    })
}

/// The direction the port was opened with, or 0 if it is not open.
///
/// Every consumer needs it: `clockwork_port_list` hands back both directions
/// mixed, and a caller that means to read sources cannot tell which of them
/// it may read without keeping its own shadow record of a fact the substrate
/// already knows.
///
/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_direction(ports: *const Ports, port: c_uint) -> c_int {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().direction as c_int)
    })
}

// ── The audio thread ────────────────────────────────────────────────────────
//
// Pointers become slices here, once, and registry.rs does the rest. A null
// channel pointer is `None`; more channels than a ring can have are ignored
// rather than indexed.

const MAX_CH: usize = crate::ring::MAX_CHANNELS as usize;

/// # Safety
/// `ports` is null or a live table;
/// `out` points to `channels` pointers, each valid for `frames` writes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_read(
    ports: *const Ports, port: c_uint, out: *const *mut f32, channels: c_uint, frames: c_uint,
) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
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
        ports.read(port, &mut dst, frames)
    })
}

/// # Safety
/// `ports` is null or a live table;
/// `input` points to `channels` pointers, each valid for `frames` reads.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_write(
    ports: *const Ports, port: c_uint, input: *const *const f32, channels: c_uint, frames: c_uint,
) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
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
        ports.write(port, &src, frames)
    })
}

// ── The endpoint's side ─────────────────────────────────────────────────────

/// # Safety
/// `ports` is null or a live table;
/// `interleaved` is valid for `frames * clockwork_port_channels(port)` reads.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_produce(
    ports: *const Ports, port: c_uint, interleaved: *const f32, frames: c_uint,
) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        if interleaved.is_null() { return 0; }
        let ch = ports.channels(port) as usize;
        // SAFETY: valid for `frames * channels` reads per the contract.
        let src = unsafe { slice::from_raw_parts(interleaved, frames as usize * ch) };
        ports.produce(port, src)
    })
}

/// # Safety
/// `ports` is null or a live table;
/// `interleaved` is valid for `frames * clockwork_port_channels(port)` writes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_consume(
    ports: *const Ports, port: c_uint, interleaved: *mut f32, frames: c_uint,
) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        if interleaved.is_null() { return 0; }
        let ch = ports.channels(port) as usize;
        // SAFETY: valid for `frames * channels` writes per the contract.
        let dst = unsafe { slice::from_raw_parts_mut(interleaved, frames as usize * ch) };
        ports.consume(port, dst)
    })
}

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_writable(ports: *const Ports, port: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().ring.writable() as u32)
    })
}

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_readable(ports: *const Ports, port: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().ring.readable() as u32)
    })
}

/// Frames read since the port opened, wrapping at 2^32: 0 for a closed port.
///
/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_read_position(ports: *const Ports, port: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().ring.read_position() as u32)
    })
}

/// Frames written since the port opened, wrapping at 2^32: 0 for a closed port.
///
/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_write_position(ports: *const Ports, port: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().ring.write_position() as u32)
    })
}

// ── Health ──────────────────────────────────────────────────────────────────

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_underruns(ports: *const Ports, port: c_uint) -> u64 {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().underruns.load(Ordering::Relaxed))
    })
}

/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_overruns(ports: *const Ports, port: c_uint) -> u64 {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        ports.acquire(port).map_or(0, |h| h.get().overruns.load(Ordering::Relaxed))
    })
}

/// # Safety
/// `ports` is null or a live table;
/// `out` is valid for `cap` writes, or NULL when `cap` is 0.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_list(ports: *const Ports, out: *mut c_uint, cap: c_uint) -> c_uint {
    no_unwind(0, || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return 0 };
        // SAFETY: the contract above is `ports.list`'s.
        unsafe { ports.list(out, cap) }
    })
}

/// The name given at open, or NULL.
///
/// The pointer is the port's own `CString` and lives exactly as long as the
/// port does, which is what the header promises ("valid until the port
/// closes"). It is therefore a diagnostics call and not one to hold onto:
/// nothing here can keep a name alive past the close that frees it.
///
/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_name(ports: *const Ports, port: c_uint) -> *const c_char {
    no_unwind(core::ptr::null(), || {
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return core::ptr::null() };
        match ports.acquire(port) {
            Some(h) => h.get().name.as_ptr(),
            None => core::ptr::null(),
        }
    })
}

/// Close every open port. Not in `clockwork_ports.h`; a test and shutdown helper, so
/// one case cannot leak slots into the next.
///
/// # Safety
/// `ports` is null or a live table.
#[no_mangle]
pub unsafe extern "C" fn clockwork_port_close_all(ports: *const Ports) {
    no_unwind((), || {
        let _scope = clockwork_heap::router::Door::enter();   // what this opens or frees is the router's
        // SAFETY: a live table, or null, per the contract.
        let Some(ports) = (unsafe { ports.as_ref() }) else { return };
        ports.close_all();
    })
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
        let ports = clockwork_ports_new();
        static SEEN: AtomicU64 = AtomicU64::new(0);
        unsafe extern "C" fn on_write(port: u32, ctx: *mut core::ffi::c_void) {
            // SAFETY: the context is the case's table, live while it runs.
            SEEN.store(unsafe { clockwork_port_readable(ctx.cast_const().cast(), port) } as u64, Ordering::Relaxed);
        }
        // SAFETY: a live table, which the case frees at its end; a
        // NUL-terminated literal; one channel pointer to 32 live frames, and
        // one to 32 writable frames.
        unsafe {
            let h = clockwork_port_open(ports, c"sig".as_ptr(), DIR_SINK as c_int, 1, 64);
            assert_eq!(clockwork_port_set_signal(ports, h, Some(on_write), ports.cast_mut().cast()), 1);
            let block = [0.5f32; 32];
            let ptrs = [block.as_ptr()];
            assert_eq!(clockwork_port_write(ports, h, ptrs.as_ptr(), 1, 32), 32);
            assert_eq!(SEEN.load(Ordering::Relaxed), 32);
            // A read on a sink is a category error and moves nothing, so it
            // does not signal either.
            let mut out = [0f32; 32];
            let optrs = [out.as_mut_ptr()];
            SEEN.store(99, Ordering::Relaxed);
            assert_eq!(clockwork_port_read(ports, h, optrs.as_ptr(), 1, 32), 0);
            assert_eq!(SEEN.load(Ordering::Relaxed), 99);
            clockwork_port_close(ports, h);
            clockwork_ports_free(ports);
        }
    }

    /// A read with no destination takes the frames and drops them, as one
    /// into buffers would, and counts no underrun for frames that were there.
    #[test]
    fn a_read_with_no_destination_takes_the_frames_and_drops_them() {
        let ports = clockwork_ports_new();
        // SAFETY: a live table, freed at the end; a NUL-terminated literal;
        // 40 interleaved frames of 2 channels; a null `out`, and two channel
        // entries that are both null, are allowed.
        unsafe {
            let h = clockwork_port_open(ports, c"drop".as_ptr(), DIR_SOURCE as c_int, 2, 64);
            let frames = [0.25f32; 40 * 2];
            assert_eq!(clockwork_port_produce(ports, h, frames.as_ptr(), 40), 40);
            assert_eq!(clockwork_port_read(ports, h, core::ptr::null(), 2, 30), 30);
            assert_eq!(clockwork_port_readable(ports, h), 10);
            let none: [*mut f32; 2] = [core::ptr::null_mut(); 2];
            assert_eq!(clockwork_port_read(ports, h, none.as_ptr(), 2, 10), 10);
            assert_eq!(clockwork_port_readable(ports, h), 0);
            assert_eq!(clockwork_port_underruns(ports, h), 0);
            clockwork_port_close(ports, h);
            clockwork_ports_free(ports);
        }
    }

    /// A port's handle names a slot in its own table and in no other: the
    /// same handle in a second table is a port that is not open there.
    #[test]
    fn a_handle_names_a_port_in_its_own_table_only() {
        let (mine, theirs) = (clockwork_ports_new(), clockwork_ports_new());
        // SAFETY: two live tables, freed at the end; a NUL-terminated literal.
        unsafe {
            let h = clockwork_port_open(mine, c"mine".as_ptr(), DIR_SINK as c_int, 2, 64);
            assert_ne!(h, 0);
            assert_eq!(clockwork_port_is_open(mine, h), 1);
            assert_eq!(clockwork_port_is_open(theirs, h), 0);
            assert_eq!(clockwork_port_channels(theirs, h), 0);
            assert_eq!(clockwork_port_is_open(core::ptr::null(), h), 0);
            clockwork_ports_free(theirs);
            assert_eq!(clockwork_port_is_open(mine, h), 1);
            clockwork_port_close(mine, h);
            clockwork_ports_free(mine);
        }
    }

    /// Two ports over one block of memory, a sink on one "side" and a source
    /// on the other, are the whole transport of the plugin bridge in
    /// miniature: what the audio thread writes into the sink, the far side's
    /// `clockwork_port_consume` reads, and the geometry is what
    /// `clockwork_port_shared_bytes` said it would be.
    #[test]
    fn a_shared_sink_and_a_shared_source_meet_in_the_same_memory() {
        let ports = clockwork_ports_new();
        let bytes = clockwork_port_shared_bytes(2, 256);
        assert_eq!(bytes, 128 + 256 * 2 * 4);
        assert_eq!(clockwork_port_shared_bytes(0, 256), 0);
        assert_eq!(clockwork_port_shared_bytes(2, 200), 0, "not a power of two");
        #[repr(align(64))]
        struct Aligned([u8; 128 + 256 * 2 * 4]);
        let mut mem = Aligned([0u8; 128 + 256 * 2 * 4]);
        let base = mem.0.as_mut_ptr();
        let name = c"shared".as_ptr();
        // SAFETY: a live table, freed at the end; `mem` is 64-aligned, `bytes`
        // long, and outlives both ports; two channel pointers to 64 live
        // frames each; `out` holds 64 frames of 2 channels.
        unsafe {
            let sink = clockwork_port_open_shared(ports, name, DIR_SINK as c_int, 2, 256, base, bytes, 1);
            let src = clockwork_port_open_shared(ports, name, DIR_SOURCE as c_int, 2, 256, base, bytes, 0);
            assert_ne!(sink, 0);
            assert_ne!(src, 0);
            let l = [1.0f32; 64];
            let r = [2.0f32; 64];
            let planar = [l.as_ptr(), r.as_ptr()];
            assert_eq!(clockwork_port_write(ports, sink, planar.as_ptr(), 2, 64), 64);
            // The far side sees the block through the source's counters.
            assert_eq!(clockwork_port_readable(ports, src), 64);
            let mut out = [0f32; 64 * 2];
            assert_eq!(clockwork_port_consume(ports, sink, out.as_mut_ptr(), 64), 64);
            assert_eq!(out[0], 1.0);
            assert_eq!(out[1], 2.0);
            assert_eq!(clockwork_port_readable(ports, src), 0);
            clockwork_port_close(ports, sink);
            clockwork_port_close(ports, src);
            clockwork_ports_free(ports);
        }
    }
}
