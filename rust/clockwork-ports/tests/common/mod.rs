// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The port calls the tests exercise, through the C ABI the engine uses, on
//! one table the process makes for them: a method per call, the table
//! filled in. Safe where the call takes no pointer of the caller's; where it
//! does, the caller vouches for its buffers, as it would for the C call.
#![allow(dead_code)]   // each test file uses some of the calls

use std::os::raw::{c_char, c_int, c_uint};
use std::sync::OnceLock;

use clockwork_ports::ffi::*;
use clockwork_ports::registry::Ports;

pub struct Table(*const Ports);
// SAFETY: a `Ports` may be shared between threads (its slot table is
// `Sync`), and a `Table` only ever names the one made below, which lives for
// the process.
unsafe impl Send for Table {}
// SAFETY: as above.
unsafe impl Sync for Table {}

/// The table, made once and never freed.
pub fn t() -> &'static Table {
    static T: OnceLock<Table> = OnceLock::new();
    T.get_or_init(|| Table(clockwork_ports_new()))
}

impl Table {
    /// # Safety
    /// `name` is a NUL-terminated C string or null.
    pub unsafe fn open(&self, name: *const c_char, dir: c_int, channels: c_uint, cap: c_uint) -> c_uint {
        // SAFETY: the table lives for the process; `name` per the contract.
        unsafe { clockwork_port_open(self.0, name, dir, channels, cap) }
    }
    /// # Safety
    /// `interleaved` holds `frames` frames of the port's channels.
    pub unsafe fn produce(&self, port: c_uint, interleaved: *const f32, frames: c_uint) -> c_uint {
        // SAFETY: the table lives for the process; the buffer per the contract.
        unsafe { clockwork_port_produce(self.0, port, interleaved, frames) }
    }
    /// # Safety
    /// `interleaved` has room for `frames` frames of the port's channels.
    pub unsafe fn consume(&self, port: c_uint, interleaved: *mut f32, frames: c_uint) -> c_uint {
        // SAFETY: as `produce`.
        unsafe { clockwork_port_consume(self.0, port, interleaved, frames) }
    }
    /// # Safety
    /// `out` is null or holds `channels` pointers, each null or to `frames` writable floats.
    pub unsafe fn read(&self, port: c_uint, out: *const *mut f32, channels: c_uint, frames: c_uint) -> c_uint {
        // SAFETY: as `produce`.
        unsafe { clockwork_port_read(self.0, port, out, channels, frames) }
    }
    /// # Safety
    /// `input` is null or holds `channels` pointers, each null or to `frames` floats.
    pub unsafe fn write(&self, port: c_uint, input: *const *const f32, channels: c_uint, frames: c_uint) -> c_uint {
        // SAFETY: as `produce`.
        unsafe { clockwork_port_write(self.0, port, input, channels, frames) }
    }
    /// # Safety
    /// `out` is valid for `cap` writes, or null when `cap` is 0.
    pub unsafe fn list(&self, out: *mut c_uint, cap: c_uint) -> c_uint {
        // SAFETY: as `produce`.
        unsafe { clockwork_port_list(self.0, out, cap) }
    }
    pub fn close(&self, port: c_uint) {
        // SAFETY: the table lives for the process.
        unsafe { clockwork_port_close(self.0, port) }
    }
    pub fn channels(&self, port: c_uint) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_port_channels(self.0, port) }
    }
    pub fn is_open(&self, port: c_uint) -> c_int {
        // SAFETY: as `close`.
        unsafe { clockwork_port_is_open(self.0, port) }
    }
    pub fn direction(&self, port: c_uint) -> c_int {
        // SAFETY: as `close`.
        unsafe { clockwork_port_direction(self.0, port) }
    }
    pub fn writable(&self, port: c_uint) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_port_writable(self.0, port) }
    }
    pub fn readable(&self, port: c_uint) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_port_readable(self.0, port) }
    }
    pub fn read_position(&self, port: c_uint) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_port_read_position(self.0, port) }
    }
    pub fn write_position(&self, port: c_uint) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_port_write_position(self.0, port) }
    }
    pub fn underruns(&self, port: c_uint) -> u64 {
        // SAFETY: as `close`.
        unsafe { clockwork_port_underruns(self.0, port) }
    }
    pub fn overruns(&self, port: c_uint) -> u64 {
        // SAFETY: as `close`.
        unsafe { clockwork_port_overruns(self.0, port) }
    }
    pub fn name(&self, port: c_uint) -> *const c_char {
        // SAFETY: as `close`.
        unsafe { clockwork_port_name(self.0, port) }
    }
}
