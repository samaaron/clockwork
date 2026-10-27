// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The C ABI of the decoders: `cpp/clockwork_audio_decode.h`.
//!
//! Two calls, probe and decode, over bytes the caller holds. The decoded
//! frames go into a buffer the caller grows through a function it passes
//! (`realloc`, in clockwork_audio_file.cpp), so the buffer is the caller's
//! from the first byte and `clockwork_audio_free` stays `free`. Nothing this
//! crate allocates outlives the call.
//!
//! A panic anywhere below is a refused file, not a crash: the decoder is
//! handed hostile bytes by design, and the boundary catches what escapes.

use std::os::raw::{c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::{decode, probe, Info, Output, Refusal};

/// `ClockworkAudioInfo` without its size field; the C side copies it across.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ClockworkAudioDecodeInfo {
    pub format: u32,
    pub encoding: u32,
    pub channels: u32,
    pub sample_rate: u32,
    pub source_bits: u32,
    pub reserved: u32,
    pub frames: u64,
}

impl From<Info> for ClockworkAudioDecodeInfo {
    fn from(i: Info) -> Self {
        ClockworkAudioDecodeInfo {
            format: i.format,
            encoding: i.encoding,
            channels: i.channels,
            sample_rate: i.sample_rate,
            source_bits: i.source_bits,
            reserved: 0,
            frames: i.frames,
        }
    }
}

/// How the output buffer grows: `realloc`'s shape. Returns the new address
/// or null when it cannot, in which case the old block is still the
/// caller's. Called with a null pointer for the first block.
pub type ClockworkAudioGrow =
    Option<unsafe extern "C" fn(block: *mut c_void, bytes: usize) -> *mut c_void>;

const E_ARG: c_int = 1;

/// # Safety
/// `data` points to `len` readable bytes, or is null with `len` zero.
unsafe fn bytes_of<'a>(data: *const u8, len: usize) -> Option<&'a [u8]> {
    if data.is_null() || len == 0 {
        return None;
    }
    // SAFETY: the contract above.
    Some(unsafe { std::slice::from_raw_parts(data, len) })
}

/// What the bytes are and how long they say they are, headers alone.
/// Returns a `ClockworkStatus`.
///
/// # Safety
/// `data` points to `len` readable bytes; `info` is null or writable.
#[no_mangle]
pub unsafe extern "C" fn clockwork_audio_decode_probe(
    data: *const u8,
    len: usize,
    info: *mut ClockworkAudioDecodeInfo,
) -> c_int {
    // SAFETY: the contract above.
    let Some(bytes) = (unsafe { bytes_of(data, len) }) else {
        return E_ARG;
    };
    if info.is_null() {
        return E_ARG;
    }
    let result = catch_unwind(AssertUnwindSafe(|| probe(bytes)));
    match result {
        Ok(Ok(i)) => {
            // SAFETY: writable, per the contract.
            unsafe { *info = i.into() };
            0
        }
        Ok(Err(r)) => r.status(),
        Err(_) => E_ARG,
    }
}

/// The caller's buffer, grown through its own function.
struct Grown {
    grow: unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void,
    block: *mut f32,
    capacity: usize, // samples
    len: usize,      // samples
}

impl Output for Grown {
    fn append(&mut self, samples: &[f32]) -> bool {
        let needed = self.len + samples.len();
        if needed > self.capacity {
            let mut capacity = self.capacity.max(4096);
            while capacity < needed {
                capacity = capacity.saturating_mul(2);
            }
            // SAFETY: grow is realloc-shaped by contract; the old block stays
            // valid when it returns null.
            let next =
                unsafe { (self.grow)(self.block.cast(), capacity * std::mem::size_of::<f32>()) };
            if next.is_null() {
                return false;
            }
            self.block = next.cast();
            self.capacity = capacity;
        }
        // SAFETY: `capacity` samples are allocated at `block`, by the grow
        // above. The copy goes through a slice, so an error in this
        // arithmetic is a panic the boundary catches, never a write past the
        // block.
        let dst = unsafe { std::slice::from_raw_parts_mut(self.block, self.capacity) };
        dst[self.len..needed].copy_from_slice(samples);
        self.len = needed;
        true
    }
}

/// Decode `len` bytes into a buffer grown through `grow`, handing over no
/// more than `max_bytes` of float. On success `*out` is the buffer and
/// `info.frames` is its length in frames; on failure `*out` is whatever was
/// allocated so far, or null, and the caller frees it. Returns a
/// `ClockworkStatus`.
///
/// # Safety
/// `data` points to `len` readable bytes; `info` and `out` are writable;
/// `grow` is `realloc`-shaped.
#[no_mangle]
pub unsafe extern "C" fn clockwork_audio_decode_run(
    data: *const u8,
    len: usize,
    max_bytes: u64,
    grow: ClockworkAudioGrow,
    info: *mut ClockworkAudioDecodeInfo,
    out: *mut *mut f32,
) -> c_int {
    if out.is_null() || info.is_null() {
        return E_ARG;
    }
    // SAFETY: writable, per the contract.
    unsafe { *out = std::ptr::null_mut() };
    // SAFETY: the contract above.
    let Some(bytes) = (unsafe { bytes_of(data, len) }) else {
        return E_ARG;
    };
    let Some(grow) = grow else { return E_ARG };

    let mut buffer = Grown {
        grow,
        block: std::ptr::null_mut(),
        capacity: 0,
        len: 0,
    };
    let result = catch_unwind(AssertUnwindSafe(|| decode(bytes, max_bytes, &mut buffer)));

    // The block is the caller's whatever happened: on success it is the
    // answer, on failure it is theirs to free.
    // SAFETY: writable, per the contract.
    unsafe { *out = buffer.block };
    match result {
        Ok(Ok(i)) => {
            if buffer.block.is_null() {
                // A file of no frames still hands back a block to free, as
                // every success does: one sample's worth.
                // SAFETY: grow is realloc-shaped.
                let block = unsafe { grow(std::ptr::null_mut(), std::mem::size_of::<f32>()) };
                if block.is_null() {
                    return Refusal::NoMemory.status();
                }
                // SAFETY: writable, per the contract.
                unsafe { *out = block.cast() };
            }
            // SAFETY: writable, per the contract.
            unsafe { *info = i.into() };
            0
        }
        Ok(Err(r)) => r.status(),
        Err(_) => E_ARG,
    }
}
