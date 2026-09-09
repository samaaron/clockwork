// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `src/dsp_api.h` — the guest boundary. The host fills [`DspConfig`] and
//! [`DspHost`] and calls the entry points a guest exports; the guest answers
//! [`DspInfo`] from `dsp_describe`. See the header for every contract; this
//! file is its shape, field for field, and `tests/layout.rs` holds it to that.

use core::ffi::{c_char, c_int, c_void};

use crate::sink::{ClockworkSink, ClockworkSinkKind};

/// The guest's instance: opaque to the host, `struct Dsp` in C.
#[repr(C)]
pub struct Dsp {
    _opaque: [u8; 0],
}

/// `ClockworkClockState` (shared_memory.h): read whole with the header's
/// `readClockworkClock`, never field by field. Opaque here.
#[repr(C)]
pub struct ClockworkClockState {
    _opaque: [u8; 0],
}

/// `ClockworkChannelMapState` (shared_memory.h). Opaque here.
#[repr(C)]
pub struct ClockworkChannelMapState {
    _opaque: [u8; 0],
}

/// `DspFpEnv`: the floating-point environment of the thread `dsp_process`
/// runs on, as the host declared it.
pub const DSP_FP_ENV_UNKNOWN: i32 = 0;
pub const DSP_FP_ENV_DENORMALS_HONOURED: i32 = 1;
pub const DSP_FP_ENV_FLUSH_TO_ZERO: i32 = 2;

/// What the host hands `dsp_new`.
#[repr(C)]
pub struct DspConfig {
    pub sample_rate: f64,
    /// Frames per `dsp_process` call.
    pub block_size: u32,
    pub max_input_channels: u32,
    pub max_output_channels: u32,
    /// May be NULL.
    pub clock: *const ClockworkClockState,
    /// May be NULL when the host reserved no arena.
    pub channel_map: *const ClockworkChannelMapState,
    /// The guest's shared-memory window; NULL/0 if none.
    pub shm_window: *mut c_void,
    pub shm_window_bytes: u32,
    pub persistent: *mut c_void,
    pub persistent_bytes: u32,
    /// The fast tier.
    pub arena: *mut c_void,
    pub arena_bytes: u32,
    /// The bulk tier; NULL/0 on a single-region host.
    pub arena_bulk: *mut c_void,
    pub arena_bulk_bytes: u32,
    /// Bulk in: the client writes, the guest only reads.
    pub inbox: *const c_void,
    pub inbox_bytes: u32,
    /// Bulk out: the guest writes, the client reads.
    pub outbox: *mut c_void,
    pub outbox_bytes: u32,
    /// Opaque to clockwork; the host's and the guest's private matter.
    pub guest_config: *const c_void,
    pub guest_config_bytes: u32,
    pub deterministic_seed: i32,
    /// A `DSP_FP_ENV_*` value.
    pub fp_env: i32,
}

/// What a guest may call back into. Every pointer may be NULL and must be
/// checked: a host that offers no sinks leaves `open_sink` NULL. `Copy`,
/// because a guest keeps its own copy: the host's pointer is only promised
/// for the duration of `dsp_new`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DspHost {
    pub ctx: *mut c_void,
    /// Reply to `origin`, or notify listeners with origin 0. Non-zero if queued.
    pub emit_osc:
        Option<unsafe extern "C" fn(ctx: *mut c_void, origin: u32, bytes: *const u8, len: u32) -> c_int>,
    pub log: Option<unsafe extern "C" fn(ctx: *mut c_void, level: c_int, text: *const c_char)>,
    /// Control thread only.
    pub open_sink: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            kind: ClockworkSinkKind,
            target: *const c_char,
            capacity: u32,
        ) -> ClockworkSink,
    >,
    /// Audio thread callable. `when` is an OSC timetag; 1 means now.
    pub send_sink: Option<
        unsafe extern "C" fn(
            ctx: *mut c_void,
            sink: ClockworkSink,
            bytes: *const u8,
            len: u32,
            when: i64,
        ) -> c_int,
    >,
    pub free_bytes: Option<unsafe extern "C" fn(ctx: *mut c_void, ptr: *mut c_void)>,
    /// The guest is done with an asset `dsp_asset` handed it.
    pub asset_release: Option<unsafe extern "C" fn(ctx: *mut c_void, id: u32) -> c_int>,
}

/// What `dsp_describe` answers. Static: describable before any instance.
#[repr(C)]
pub struct DspInfo {
    pub name: *const c_char,
    pub version: *const c_char,
    /// Non-zero: the guest holds its own schedule and wants every message the
    /// moment it arrives, carrying its timetag.
    pub holds_schedule: i32,
    /// Bytes wanted from the fast tier; 0 means no claim.
    pub arena_bytes_wanted: u32,
    /// Bytes wanted from the bulk tier; 0 means no claim.
    pub arena_bulk_bytes_wanted: u32,
    /// Non-zero: the host's inbound events (`/clockwork/midi/in/…`,
    /// `/clockwork/gamepad/in/…`, the ports and devices pushes) are handed
    /// to `dsp_osc` as they arrive, each with its arrival timetag as a
    /// trailing `t` argument when the host knows it.
    pub wants_events: i32,
}

// SAFETY: a DspInfo is immutable static data — string pointers into
// statics — which is the only way a guest ever builds one.
unsafe impl Sync for DspInfo {}

/// `CLOCKWORK_ASSET_*`: what an asset's bytes are.
pub const CLOCKWORK_ASSET_RAW: u32 = 0;
pub const CLOCKWORK_ASSET_AUDIO_F32: u32 = 1;

/// One asset the client staged in the inbox and committed, handed to
/// `dsp_asset` on the audio thread. The bytes stay valid until the guest
/// calls `DspHost::asset_release(id)`.
#[repr(C)]
pub struct ClockworkAsset {
    /// `sizeof(ClockworkAsset)`, for the guest to check.
    pub struct_bytes: u32,
    pub id: u32,
    /// A `CLOCKWORK_ASSET_*` value.
    pub kind: u32,
    /// Who committed it; opaque to the guest.
    pub origin: u32,
    pub bytes: *const c_void,
    pub byte_count: u32,
    /// `AUDIO_F32` only, else 0.
    pub channels: u32,
    pub frames: u32,
    pub sample_rate: f32,
}

// ── The entry points a guest exports ─────────────────────────────────────────
//
// Declared as function TYPES rather than functions: a guest defines them with
// `#[no_mangle] pub unsafe extern "C" fn dsp_new(...)` and can assert its
// definition against these to be sure of the signature:
//
//     const _: clockwork_abi::dsp_api::DspNewFn = dsp_new;

pub type DspDescribeFn = unsafe extern "C" fn() -> *const DspInfo;
pub type DspNewFn =
    unsafe extern "C" fn(config: *const DspConfig, host: *const DspHost, err: *mut *const c_char) -> *mut Dsp;
pub type DspFreeFn = unsafe extern "C" fn(dsp: *mut Dsp);
pub type DspProcessFn = unsafe extern "C" fn(
    dsp: *mut Dsp,
    inputs: *const *const f32,
    n_in: u32,
    outputs: *const *mut f32,
    n_out: u32,
    frames: u32,
    block_time: i64,
);
pub type DspOscFn = unsafe extern "C" fn(
    dsp: *mut Dsp,
    bytes: *const u8,
    len: u32,
    when: i64,
    origin: u32,
    block_time: i64,
);
/// Return 0 to take the asset, non-zero to refuse it.
pub type DspAssetFn = unsafe extern "C" fn(dsp: *mut Dsp, asset: *const ClockworkAsset) -> c_int;
