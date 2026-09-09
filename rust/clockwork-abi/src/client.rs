// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `src/clockwork_client.h` — the client boundary: a handle over an engine's
//! memory, wherever that memory is (another process's segment, this address
//! space, a wasm heap), and the calls a client makes through it. The header
//! is the contract; this is its shape.

use core::ffi::{c_char, c_int, c_void};

pub const CLOCKWORK_CLIENT_ABI_VERSION: u32 = 1;

/// `ClockworkStatus`. A newtype, not an enum: a status the header does not
/// list must be a value here, not undefined behaviour.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct ClockworkStatus(pub c_int);

impl ClockworkStatus {
    pub const OK: ClockworkStatus = ClockworkStatus(0);
    pub const E_ARG: ClockworkStatus = ClockworkStatus(1);
    pub const E_VERSION: ClockworkStatus = ClockworkStatus(2);
    pub const E_NOT_FOUND: ClockworkStatus = ClockworkStatus(3);
    pub const E_PERM: ClockworkStatus = ClockworkStatus(4);
    pub const E_ABSENT: ClockworkStatus = ClockworkStatus(5);
    pub const E_FULL: ClockworkStatus = ClockworkStatus(6);
    pub const E_TOO_BIG: ClockworkStatus = ClockworkStatus(7);
    pub const E_CLOSED: ClockworkStatus = ClockworkStatus(8);
    pub const E_NOMEM: ClockworkStatus = ClockworkStatus(9);

    pub fn is_ok(self) -> bool { self == Self::OK }
}

/// The handle. Opaque; `ClockworkClient` in C.
#[repr(C)]
pub struct ClockworkClient {
    _opaque: [u8; 0],
}

/// A tap on a ring: a reader with a cursor of its own that consumes nothing.
#[repr(C)]
pub struct ClockworkClientTap {
    _opaque: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ClockworkClientInfo {
    pub struct_bytes: u32,
    pub abi_version: u32,
    pub engine_version: u32,
    /// `CLOCKWORK_FEATURE_*` bits.
    pub features: u32,
    pub sample_rate: f64,
    pub block_frames: u32,
    pub input_channels: u32,
    pub output_channels: u32,
}

pub const CLOCKWORK_FEATURE_SCOPE: u32 = 1 << 0;
pub const CLOCKWORK_FEATURE_AUDIO_TAPS: u32 = 1 << 1;
pub const CLOCKWORK_FEATURE_BULK: u32 = 1 << 2;
pub const CLOCKWORK_FEATURE_WINDOW: u32 = 1 << 3;
pub const CLOCKWORK_FEATURE_SCHEDULER: u32 = 1 << 4;

/// One frame off the egress, as `clockwork_client_poll` hands it over. The
/// bytes are the ring's and are valid until the next poll.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkClientMessage {
    pub bytes: *const u8,
    pub length: u32,
    /// The origin it answers; 0 for a broadcast.
    pub origin: u32,
    /// A `CLOCKWORK_ROUTE_*` word (the EgressRoute the engine sent it by).
    pub route: u32,
    pub sequence: u32,
}

pub const CLOCKWORK_ROUTE_REPLY: u32 = 0;
pub const CLOCKWORK_ROUTE_NOTIFY: u32 = 2;

/// `ClockworkRegionId`. A newtype, as [`ClockworkStatus`] is.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkRegionId(pub c_int);

impl ClockworkRegionId {
    pub const METRICS: ClockworkRegionId = ClockworkRegionId(1);
    pub const WINDOW: ClockworkRegionId = ClockworkRegionId(2);
    pub const INBOX: ClockworkRegionId = ClockworkRegionId(3);
    pub const OUTBOX: ClockworkRegionId = ClockworkRegionId(4);
    pub const SCOPE: ClockworkRegionId = ClockworkRegionId(5);
    pub const AUDIO_TAPS: ClockworkRegionId = ClockworkRegionId(6);
    pub const INGRESS: ClockworkRegionId = ClockworkRegionId(7);
    pub const EGRESS: ClockworkRegionId = ClockworkRegionId(8);
    pub const NATIVE_STATS: ClockworkRegionId = ClockworkRegionId(9);
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkRegion {
    pub base: *mut c_void,
    pub bytes: u32,
    pub writable: c_int,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ClockworkClientClock {
    pub struct_bytes: u32,
    pub bpm: f64,
    pub beat_origin_ntp: f64,
    pub is_playing_at_ntp: f64,
    pub is_playing: i32,
    pub flags: u32,
    pub meter_num: i32,
    pub meter_den: i32,
}

/// A scope slot reader. Fill `struct_bytes` with `size_of::<Self>()` and
/// `slot`; `clockwork_client_scope_open` fills the rest.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkScopeReader {
    pub struct_bytes: u32,
    pub slot: u32,
    pub _internal: *mut c_void,
    pub _ring_frames: u32,
}

impl ClockworkScopeReader {
    /// A reader for `slot`, ready to hand to `clockwork_client_scope_open`.
    pub const fn for_slot(slot: u32) -> ClockworkScopeReader {
        ClockworkScopeReader {
            struct_bytes: core::mem::size_of::<ClockworkScopeReader>() as u32,
            slot,
            _internal: core::ptr::null_mut(),
            _ring_frames: 0,
        }
    }
}

extern "C" {
    pub fn clockwork_client_abi_version() -> u32;
    pub fn clockwork_client_status_text(s: ClockworkStatus) -> *const c_char;

    pub fn clockwork_client_open_shm(endpoint: *const c_char, out_status: *mut ClockworkStatus) -> *mut ClockworkClient;
    pub fn clockwork_client_default_endpoint(port: u32, buf: *mut c_char, cap: u32) -> u32;
    pub fn clockwork_client_open_shm_handle(native_handle: isize, out_status: *mut ClockworkStatus) -> *mut ClockworkClient;
    pub fn clockwork_client_open_memory(base: *mut c_void, bytes: u32, out_status: *mut ClockworkStatus) -> *mut ClockworkClient;
    pub fn clockwork_client_sizeof() -> usize;
    pub fn clockwork_client_open_memory_in(
        storage: *mut c_void,
        storage_bytes: usize,
        base: *mut c_void,
        bytes: u32,
        out_status: *mut ClockworkStatus,
    ) -> *mut ClockworkClient;
    pub fn clockwork_client_close(c: *mut ClockworkClient);

    pub fn clockwork_client_info(c: *mut ClockworkClient, out: *mut ClockworkClientInfo) -> ClockworkStatus;

    pub fn clockwork_client_send(c: *mut ClockworkClient, osc: *const u8, bytes: u32, origin: u32) -> ClockworkStatus;
    pub fn clockwork_client_send_begin(c: *mut ClockworkClient, max_bytes: u32, out_status: *mut ClockworkStatus) -> *mut u8;
    pub fn clockwork_client_send_commit(c: *mut ClockworkClient, bytes: u32, origin: u32) -> ClockworkStatus;
    pub fn clockwork_client_send_abort(c: *mut ClockworkClient);
    pub fn clockwork_client_poll(c: *mut ClockworkClient, out: *mut ClockworkClientMessage, max: u32) -> u32;

    pub fn clockwork_client_tap_sizeof() -> usize;
    pub fn clockwork_client_tap_open(c: *mut ClockworkClient, ring: u32, out_status: *mut ClockworkStatus) -> *mut ClockworkClientTap;
    pub fn clockwork_client_tap_open_in(
        storage: *mut c_void,
        storage_bytes: usize,
        c: *mut ClockworkClient,
        ring: u32,
        out_status: *mut ClockworkStatus,
    ) -> *mut ClockworkClientTap;
    pub fn clockwork_client_tap_close(t: *mut ClockworkClientTap);
    pub fn clockwork_client_tap_poll(t: *mut ClockworkClientTap, out: *mut ClockworkClientMessage, max: u32) -> u32;
    pub fn clockwork_client_tap_missed(t: *const ClockworkClientTap) -> u64;

    pub fn clockwork_client_region(c: *mut ClockworkClient, id: ClockworkRegionId, out: *mut ClockworkRegion) -> ClockworkStatus;
    pub fn clockwork_client_metrics(c: *mut ClockworkClient, out: *mut u32, max: u32) -> u32;
    pub fn clockwork_client_clock(c: *mut ClockworkClient, out: *mut ClockworkClientClock) -> ClockworkStatus;
    pub fn clockwork_client_beat_at(clock: *const ClockworkClientClock, ntp_seconds: f64) -> f64;

    pub fn clockwork_client_scope_open(c: *mut ClockworkClient, slot: u32, out: *mut ClockworkScopeReader) -> ClockworkStatus;
    pub fn clockwork_client_scope_valid(r: *const ClockworkScopeReader) -> c_int;
    pub fn clockwork_client_scope_audible_end(c: *mut ClockworkClient, r: *const ClockworkScopeReader) -> u64;
    pub fn clockwork_client_scope_read(
        c: *mut ClockworkClient,
        r: *const ClockworkScopeReader,
        end: u64,
        frames: u32,
        out: *mut f32,
        out_channels: *mut u32,
    ) -> u32;
}
