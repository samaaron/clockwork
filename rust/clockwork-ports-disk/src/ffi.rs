// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The C ABI of the disk endpoints — `cpp/clockwork_disk.h`.
//!
//! Deliberately tiny, and deliberately its own header rather than an addition
//! to `clockwork_ports.h`: the substrate's header would stop being generic the
//! moment the word "path" appeared in it. A second endpoint (a network peer, a
//! plugin, a capture buffer) gets its own header in the same shape and the
//! substrate is untouched, which is the property the whole exercise is for.
//!
//! A handle is an opaque pointer rather than a slot number. Endpoints are not
//! addressed by anyone but their owner, do not need to be enumerable, and do
//! not need to survive their owner — none of the reasons a port has a slot
//! apply, and inventing a second registry for them would be the mistake ports
//! were built to stop.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_uint};
use std::path::PathBuf;

use crate::sink::DiskSink;
use crate::source::DiskSource;

/// Opaque to C.
pub struct ClockworkDiskSource(DiskSource);
/// Opaque to C.
pub struct ClockworkDiskSink(DiskSink);

/// # Safety
/// `p` is null or a NUL-terminated C string.
unsafe fn path_of(p: *const c_char) -> Option<PathBuf> {
    if p.is_null() { return None; }
    // SAFETY: NUL-terminated, per the contract.
    let c = unsafe { CStr::from_ptr(p) };
    #[cfg(unix)]
    {
        use std::os::unix::ffi::OsStrExt;
        Some(PathBuf::from(std::ffi::OsStr::from_bytes(c.to_bytes())))
    }
    #[cfg(not(unix))]
    {
        Some(PathBuf::from(c.to_string_lossy().into_owned()))
    }
}

/// Start streaming `path` into `port`, which must be an open SOURCE.
/// NULL if the file cannot be opened, is not a WAV this reader understands, or
/// the port is not open.
///
/// # Safety
/// `path` is a NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_source_open(
    path: *const c_char, port: c_uint,
) -> *mut ClockworkDiskSource {
    // SAFETY: the contract above is `path_of`'s.
    let Some(p) = (unsafe { path_of(path) }) else { return core::ptr::null_mut() };
    match DiskSource::open(&p, port) {
        Ok(s) => Box::into_raw(Box::new(ClockworkDiskSource(s))),
        Err(_) => core::ptr::null_mut(),
    }
}

/// Stop the worker and free the handle. Does not close the port: the port
/// outlives its endpoint, which is what lets an endpoint be swapped under a
/// running stream.
///
/// # Safety
/// `h` came from `clockwork_disk_source_open` and is not used again.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_source_close(h: *mut ClockworkDiskSource) {
    if h.is_null() { return; }
    // SAFETY: the box the open call leaked, freed once, per the contract.
    unsafe { drop(Box::from_raw(h)) };
}

/// Frames in the file, from its header. 0 for a NULL handle.
///
/// # Safety
/// `h` is a live handle or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_source_frames(h: *const ClockworkDiskSource) -> u64 {
    if h.is_null() { return 0; }
    // SAFETY: a live handle, per the contract.
    unsafe { (*h).0.total_frames() }
}

/// The file's own sample rate. NOT converted to the session's — see
/// `src/source.rs`; a caller that cares must compare it itself.
///
/// # Safety
/// `h` is a live handle or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_source_sample_rate(h: *const ClockworkDiskSource) -> c_uint {
    if h.is_null() { return 0; }
    // SAFETY: a live handle, per the contract.
    unsafe { (*h).0.sample_rate() }
}

/// Frames handed to the port so far.
///
/// # Safety
/// `h` is a live handle or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_source_frames_produced(h: *const ClockworkDiskSource) -> u64 {
    if h.is_null() { return 0; }
    // SAFETY: a live handle, per the contract.
    unsafe { (*h).0.frames_produced() }
}

/// Non-zero once the whole file has been handed to the port. The port then
/// underruns to silence, which is how a stream ends.
///
/// # Safety
/// `h` is a live handle or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_source_eof(h: *const ClockworkDiskSource) -> c_int {
    if h.is_null() { return 1; }
    // SAFETY: a live handle, per the contract.
    unsafe { (*h).0.at_eof() as c_int }
}

/// Start draining `port` (an open SINK) into a new 32-bit float WAV at `path`.
///
/// # Safety
/// `path` is a NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_sink_open(
    path: *const c_char, port: c_uint, sample_rate: c_uint, channels: c_uint,
) -> *mut ClockworkDiskSink {
    // SAFETY: the contract above is `path_of`'s.
    let Some(p) = (unsafe { path_of(path) }) else { return core::ptr::null_mut() };
    if channels == 0 || channels > u16::MAX as u32 { return core::ptr::null_mut(); }
    match DiskSink::open(&p, port, sample_rate, channels as u16) {
        Ok(s) => Box::into_raw(Box::new(ClockworkDiskSink(s))),
        Err(_) => core::ptr::null_mut(),
    }
}

/// Drain what is left, patch the header with the real length, join the worker,
/// free the handle. The file is only complete after this returns.
///
/// # Safety
/// `h` came from `clockwork_disk_sink_open` and is not used again.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_sink_close(h: *mut ClockworkDiskSink) {
    if h.is_null() { return; }
    // SAFETY: the box the open call leaked, freed once, per the contract.
    unsafe { drop(Box::from_raw(h)) };
}

/// Frames written to the file so far.
///
/// # Safety
/// `h` is a live handle or NULL.
#[no_mangle]
pub unsafe extern "C" fn clockwork_disk_sink_frames(h: *const ClockworkDiskSink) -> u64 {
    if h.is_null() { return 0; }
    // SAFETY: a live handle, per the contract.
    unsafe { (*h).0.frames_written() }
}
