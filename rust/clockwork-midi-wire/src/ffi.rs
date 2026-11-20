// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The C ABI of `src/midi_wire.h`. Audio-thread safe: nothing here allocates,
//! locks or waits.
//!
//! # Panic freedom across the boundary
//!
//! Every pointer is null-checked and every length bounds what is read or
//! written; a port name that is not UTF-8 converts nothing. A panic reaching
//! an `extern "C"` frame aborts (Rust 1.81), and `no_unwind` answers "nothing
//! converted" instead where the build unwinds.

use clockwork_fence::no_unwind;
use core::ffi::{c_char, CStr};

use crate::{decode_out, encode_in, encode_ports, MAX_MIDI_BYTES};

/// An outbound send, as `clockwork_midi_wire_out` reads it. `port` points
/// into the message it was read from and is valid as long as that is; it is
/// NUL-terminated there.
#[repr(C)]
pub struct ClockworkMidiWireOut {
    pub port: *const c_char,
    pub when: u64,
    pub bytes: [u8; MAX_MIDI_BYTES],
    pub len: u8,
    pub every_channel: u32,
}

/// # Safety
/// `p` is null or a NUL-terminated string valid for the call.
unsafe fn name<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    // SAFETY: not null, and the caller's NUL-terminated string.
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// # Safety
/// `p` is null or valid for `len` bytes for the call.
unsafe fn slice<'a>(p: *const u8, len: u32) -> Option<&'a [u8]> {
    if p.is_null() {
        return None;
    }
    // SAFETY: not null, and the caller's `len` readable bytes.
    Some(unsafe { core::slice::from_raw_parts(p, len as usize) })
}

/// # Safety
/// `p` is null or valid for writes of `cap` bytes for the call.
unsafe fn slice_mut<'a>(p: *mut u8, cap: u32) -> Option<&'a mut [u8]> {
    if p.is_null() {
        return None;
    }
    // SAFETY: not null, and the caller's `cap` writable bytes.
    Some(unsafe { core::slice::from_raw_parts_mut(p, cap as usize) })
}

/// `midi[0..len)`, one short channel message from `port`, as its inbound OSC
/// form in `out[0..cap)`: the bytes written, 0 for nothing converted.
///
/// # Safety
/// `port` is null or a NUL-terminated string; `midi` is null or valid for
/// `len` bytes; `out` is null or valid for writes of `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_wire_in(
    port: *const c_char, midi: *const u8, len: u32, when: u64, out: *mut u8, cap: u32,
) -> u32 {
    no_unwind(0, || {
        // SAFETY: the caller's contract, passed on.
        let (port, midi, out) = unsafe { (name(port), slice(midi, len), slice_mut(out, cap)) };
        match (port, midi, out) {
            (Some(port), Some(midi), Some(out)) => encode_in(port, midi, when, out).unwrap_or(0) as u32,
            _ => 0,
        }
    })
}

/// The outbound send in `osc[0..len)`, read into `*send`: 1, or 0 for
/// anything that is not a send this carries, with `*send` untouched.
///
/// # Safety
/// `osc` is null or valid for `len` bytes; `send` is null or valid for a
/// write.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_wire_out(osc: *const u8, len: u32, send: *mut ClockworkMidiWireOut) -> i32 {
    no_unwind(0, || {
        // SAFETY: the caller's contract, passed on.
        let Some(osc) = (unsafe { slice(osc, len) }) else { return 0 };
        let Some(out) = decode_out(osc) else { return 0 };
        if send.is_null() {
            return 0;
        }
        // SAFETY: not null, and the caller's writable struct. The port is a
        // string inside `osc`, with the NUL that ended it there.
        unsafe {
            send.write(ClockworkMidiWireOut {
                port: out.port.as_ptr().cast(),
                when: out.when,
                bytes: out.bytes,
                len: out.len as u8,
                every_channel: u32::from(out.every_channel),
            });
        }
        1
    })
}

/// The ports a host supplies as the port list's OSC form in `out[0..cap)`:
/// an answer (`reply` non-zero) or a notice. The bytes written, 0 for no room.
///
/// # Safety
/// `input` and `output` are each null or a NUL-terminated string; `out` is
/// null or valid for writes of `cap` bytes.
#[no_mangle]
pub unsafe extern "C" fn clockwork_midi_wire_ports(
    input: *const c_char, output: *const c_char, reply: i32, out: *mut u8, cap: u32,
) -> u32 {
    no_unwind(0, || {
        // SAFETY: the caller's contract, passed on.
        let (input, output, out) = unsafe { (name(input), name(output), slice_mut(out, cap)) };
        match out {
            Some(out) => encode_ports(input, output, reply != 0, out).unwrap_or(0) as u32,
            None => 0,
        }
    })
}
