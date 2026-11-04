// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The sink calls the tests exercise, through the C ABI a caller has, on one
//! table the process makes for them: a method per call, the table filled
//! in. Safe where the call takes no pointer of the caller's; where it does,
//! the caller vouches for it, as it would for the C call. The Rust API is
//! the table's own (`sinks()`).
#![allow(dead_code)]   // each test file uses some of the calls

use std::os::raw::{c_char, c_int, c_uint};
use std::sync::{Arc, OnceLock};

use clockwork_osc::Timetag;
use clockwork_sinks::ffi::*;
use clockwork_sinks::registry::Sinks;
use clockwork_sinks::sink::Stats;
use clockwork_sinks::HostEmitter;

pub struct Table(Arc<Sinks>);

/// The table, made once and never freed.
pub fn t() -> &'static Table {
    static T: OnceLock<Table> = OnceLock::new();
    T.get_or_init(|| Table(Sinks::new(std::ptr::null_mut())))
}

impl Table {
    pub fn sinks(&self) -> &Sinks { &self.0 }
    fn raw(&self) -> *const Sinks { Arc::as_ptr(&self.0) }

    /// # Safety
    /// `target` is a NUL-terminated C string or null.
    pub unsafe fn open(&self, kind: c_int, target: *const c_char, capacity: c_uint) -> c_uint {
        // SAFETY: the table lives for the process; `target` per the contract.
        unsafe { clockwork_sink_open(self.raw(), kind, target, capacity) }
    }
    /// # Safety
    /// `bytes` is valid for `len` reads, or null when `len` is 0.
    pub unsafe fn send(&self, sink: c_uint, bytes: *const u8, len: c_uint, when: Timetag) -> c_int {
        // SAFETY: the table lives for the process; `bytes` per the contract.
        unsafe { clockwork_sink_send(self.raw(), sink, bytes, len, when) }
    }
    /// # Safety
    /// `out` points to a writable `Stats`, or is null.
    pub unsafe fn stats(&self, sink: c_uint, out: *mut Stats) -> c_int {
        // SAFETY: as `send`.
        unsafe { clockwork_sink_stats(self.raw(), sink, out) }
    }
    /// # Safety
    /// `out` is valid for `cap` writes, or null when `cap` is 0.
    pub unsafe fn list(&self, out: *mut c_uint, cap: c_uint) -> c_uint {
        // SAFETY: as `send`.
        unsafe { clockwork_sink_list(self.raw(), out, cap) }
    }
    /// # Safety
    /// `emitter` is null or valid until the next call.
    pub unsafe fn set_host_emitter(&self, emitter: *const HostEmitter) {
        // SAFETY: as `send`.
        unsafe { clockwork_sink_set_host_emitter(self.raw(), emitter) }
    }
    pub fn close(&self, sink: c_uint) {
        // SAFETY: the table lives for the process.
        unsafe { clockwork_sink_close(self.raw(), sink) }
    }
    pub fn close_all(&self) {
        // SAFETY: as `close`.
        unsafe { clockwork_sink_close_all(self.raw()) }
    }
    pub fn is_open(&self, sink: c_uint) -> c_int {
        // SAFETY: as `close`.
        unsafe { clockwork_sink_is_open(self.raw(), sink) }
    }
    pub fn kind(&self, sink: c_uint) -> c_int {
        // SAFETY: as `close`.
        unsafe { clockwork_sink_kind(self.raw(), sink) }
    }
    pub fn target(&self, sink: c_uint) -> *const c_char {
        // SAFETY: as `close`.
        unsafe { clockwork_sink_target(self.raw(), sink) }
    }
    pub fn flush(&self, sink: c_uint) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_sink_flush(self.raw(), sink) }
    }
    pub fn flush_all(&self) -> c_uint {
        // SAFETY: as `close`.
        unsafe { clockwork_sink_flush_all(self.raw()) }
    }
}
