// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! What a HOST calls (`src/lanes/lanes.h`): the engine boundary for whoever
//! owns the audio loop.
//!
//! A client (`client`) talks to an engine somebody else drives. A host IS
//! that somebody: it boots the engine once, then per block writes ingress,
//! ticks, reads the rendered block and drains egress. The browser worklet,
//! the test fixture and a plugin running in a DAW's process callback are
//! all hosts in this sense — the DAW hands the plugin a buffer and a
//! deadline, and the plugin ticks clockwork to fill it.
//!
//! Every function here is audio-thread-safe where the header says so and
//! no safer: `clockwork_init` and `clockwork_attach` allocate and belong to
//! a control thread; the rest are the per-block sequence. The engine is a
//! process singleton, which is what the attach protocol is for.

use core::ffi::{c_int, c_void};

/// One drained egress frame: the origin token the reply targets, the route
/// word, the OSC bytes (valid only for the duration of the call), and the
/// ring sequence number.
pub type ClockworkEgressFn = Option<
    unsafe extern "C" fn(ctx: *mut c_void, source_id: u32, route: u32, osc: *const u8, len: u32, seq: u32),
>;

/// What `clockwork_attach` answered.
pub const CLOCKWORK_ATTACH_REFUSED: c_int = 0;
pub const CLOCKWORK_ATTACH_BOOTED: c_int = 1;
pub const CLOCKWORK_ATTACH_JOINED: c_int = 2;

/// The most channels a host may reserve above the device for its own routing.
pub const CLOCKWORK_RESERVED_LANES_MAX: u32 = 64;

unsafe extern "C" {
    // ── Ingress ──
    pub fn clockwork_ingress_write(osc: *const u8, len: u32, source_id: u32) -> bool;

    // ── Egress ── one draining thread per ring; max_frames 0 drains all.
    pub fn clockwork_egress_rt_drain(f: ClockworkEgressFn, ctx: *mut c_void, max_frames: u32) -> u32;
    pub fn clockwork_egress_nrt_drain(f: ClockworkEgressFn, ctx: *mut c_void, max_frames: u32) -> u32;

    // ── Tick ── audio thread only. ntp_now: this block's first sample, in
    // seconds since 1900 (a host without a wall clock passes a base plus
    // elapsed samples over the rate). false only on fatal engine error.
    pub fn clockwork_tick(ntp_now: f64, out_channels: u32, in_channels: u32) -> bool;
    pub fn clockwork_audio_out() -> *const f32; // channel-major, block_size frames per channel
    pub fn clockwork_audio_in() -> *mut f32; // fill before the tick
    pub fn clockwork_block_size() -> u32;
    pub fn clockwork_sample_rate() -> f64; // 0 before init
    pub fn clockwork_block_time() -> i64; // the block in flight, OSC 32.32

    // ── Init ── the sole-host call; reconfigures a running engine.
    pub fn clockwork_init(
        sample_rate: f64,
        block_size: u32,
        input_channels: u32,
        output_channels: u32,
        verbosity: u32,
        guest_arena: *mut c_void,
        guest_arena_bytes: u32,
        guest_config: *const c_void,
        guest_config_bytes: u32,
        arena_bytes: u32,
        inbox: *const c_void,
        inbox_bytes: u32,
        outbox: *mut c_void,
        outbox_bytes: u32,
    );

    // ── More than one host in a process ── BOOTED, JOINED (your arguments
    // were not applied: read the rate and block back) or REFUSED.
    pub fn clockwork_attach(
        sample_rate: f64,
        block_size: u32,
        input_channels: u32,
        output_channels: u32,
        verbosity: u32,
        guest_arena: *mut c_void,
        guest_arena_bytes: u32,
        guest_config: *const c_void,
        guest_config_bytes: u32,
        arena_bytes: u32,
        inbox: *const c_void,
        inbox_bytes: u32,
        outbox: *mut c_void,
        outbox_bytes: u32,
    ) -> c_int;
    pub fn clockwork_detach();
    pub fn clockwork_attached() -> u32;

    pub fn clockwork_set_channel_ceilings(input_channels: u32, output_channels: u32);
    pub fn clockwork_declare_fp_env(env: u32);

    // ── Forwarding ── the host answers the subsystem verbs itself.
    pub fn clockwork_host_forward(on: c_int);
    pub fn clockwork_host_forwards() -> c_int;

    // ── Reserved lanes ──
    pub fn clockwork_reserve_lanes(lanes: u32);
    pub fn clockwork_reserved_lanes() -> u32;
    pub fn clockwork_lane_base() -> u32;

    // ── Layout ── the arena's own table (`arena`), and the base it sits at.
    pub fn clockwork_arena_header() -> *const crate::arena::ClockworkArenaHeader;
    pub fn clockwork_lanes_base() -> *mut c_void;
    pub fn clockwork_channel_map() -> *const c_void;
}
