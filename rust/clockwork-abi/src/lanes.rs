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
//! no safer: `clockwork_init` allocates and belongs to a control thread;
//! the rest are the per-block sequence. An engine is a core
//! (`src/clockwork_core.h`): the host makes one, and `clockwork_init` and
//! `clockwork_tick` take it.

use core::ffi::{c_char, c_int, c_void};

/// One drained egress frame: the origin token the reply targets, the route
/// word, the OSC bytes (valid only for the duration of the call), and the
/// ring sequence number.
pub type ClockworkEgressFn = Option<
    unsafe extern "C" fn(ctx: *mut c_void, source_id: u32, route: u32, osc: *const u8, len: u32, seq: u32),
>;

/// One engine (`ClockworkCore`, src/clockwork_core.h). Opaque.
#[repr(C)]
pub struct ClockworkCore {
    _opaque: [u8; 0],
}

/// The most channels a host may reserve above the device for its own routing.
pub const CLOCKWORK_RESERVED_LANES_MAX: u32 = 64;

/// The block's time (`ClockworkBlockTime`, lanes.h): inputs from the host,
/// outputs from the engine. No host keeps a clock of its own; the engine's
/// clock is anchored, slewed and published by the tick.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ClockworkBlockTime {
    /// `size_of::<ClockworkBlockTime>()`: the struct may grow, a host says which it built.
    pub struct_bytes: u32,
    /// In: the device's output latency in frames; 0 without a device.
    pub output_latency_frames: u32,
    /// In: the host's count of frames rendered so far, i.e. this block's first frame.
    pub audio_frames: u64,
    /// In: the host's wall clock now, NTP seconds.
    pub wall_ntp: f64,
    /// In: the host's monotonic clock now, seconds from any origin; 0 if it has none.
    pub monotonic_secs: f64,
    /// In: the audio framework's playback timestamp for this block in Link's clock; 0 if it gives none.
    pub link_host_micros: u64,
    /// Out: the engine's NTP time for the block's first frame.
    pub block_ntp: f64,
    /// Out: the block's timestamp in Link's clock (0 without Link).
    pub block_host_micros: u64,
}

/// `ClockworkTick`: what came of a tick (lanes.h). A newtype, as the
/// client's enums are.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkTick(pub c_int);

impl ClockworkTick {
    /// No core to tick: stop.
    pub const NONE: ClockworkTick = ClockworkTick(0);
    /// The block is in `clockwork_audio_out`.
    pub const RENDERED: ClockworkTick = ClockworkTick(1);
    /// Another call holds the core: no block; play silence, leave the
    /// buffers alone, tick again.
    pub const BUSY: ClockworkTick = ClockworkTick(2);
}

unsafe extern "C" {
    // ── The core ── (src/clockwork_core.h) a host makes one before
    // clockwork_init and closes it when it is done. NULL with E_PERM while
    // the process holds another.
    pub fn clockwork_core_new(status: *mut crate::client::ClockworkStatus) -> *mut ClockworkCore;
    pub fn clockwork_core_close(core: *mut ClockworkCore);
    pub fn clockwork_core_live(core: *const ClockworkCore) -> bool;
    pub fn clockwork_core_id(core: *const ClockworkCore) -> u32;
    /// The live core whose arena holds `at`, or null: how a client opened
    /// over memory in this process finds its core.
    pub fn clockwork_core_of_memory(at: *const c_void) -> *mut ClockworkCore;

    // ── Ingress ──
    pub fn clockwork_ingress_write(core: *mut ClockworkCore, osc: *const u8, len: u32, source_id: u32) -> bool;

    // ── Egress ── one draining thread per ring; max_frames 0 drains all.
    pub fn clockwork_egress_rt_drain(core: *mut ClockworkCore, f: ClockworkEgressFn, ctx: *mut c_void, max_frames: u32) -> u32;
    pub fn clockwork_egress_nrt_drain(core: *mut ClockworkCore, f: ClockworkEgressFn, ctx: *mut c_void, max_frames: u32) -> u32;

    // ── Tick ── audio thread only. `time` (ClockworkBlockTime, lanes.h)
    // carries the host's frame count, clocks, output latency and Link stamp
    // in, and gets the engine's block time back. A host with no wall clock
    // passes its own time as wall_ntp and 0 as monotonic_secs and never sees
    // a gap. Returns false only on a fatal engine error.
    pub fn clockwork_tick(core: *mut ClockworkCore, time: *mut ClockworkBlockTime, out_channels: u32, in_channels: u32) -> ClockworkTick;
    // clockwork_audio_back: called by the tick every block with the host's
    // clocks and its rendered audio in seconds; returns true when the audio
    // was away, in which case the clock re-anchors at that block. Nothing
    // that waited is dropped. clockwork_audio_starts: the host's audio is
    // starting (a device opened, an embedded host attached); the next block
    // re-anchors the clock.
    pub fn clockwork_audio_back(core: *mut ClockworkCore, wall_ntp: f64, monotonic_secs: f64, audio_secs: f64) -> bool;
    pub fn clockwork_audio_starts(core: *mut ClockworkCore, wall_ntp: f64, monotonic_secs: f64) -> bool;
    pub fn clockwork_audio_out(core: *mut ClockworkCore) -> *const f32; // channel-major, block_size frames per channel
    pub fn clockwork_audio_in(core: *mut ClockworkCore) -> *mut f32; // fill before the tick
    pub fn clockwork_out_channels(core: *mut ClockworkCore) -> u32; // 0 with no guest
    pub fn clockwork_in_channels(core: *mut ClockworkCore) -> u32; // 0 with no guest
    pub fn clockwork_block_size(core: *mut ClockworkCore) -> u32;
    pub fn clockwork_sample_rate(core: *mut ClockworkCore) -> f64; // 0 before init
    pub fn clockwork_block_time(core: *mut ClockworkCore) -> i64; // the block in flight, OSC 32.32

    // ── Init ── boots the core, or brings back the engine its arena holds.
    pub fn clockwork_init(
        core: *mut ClockworkCore,
        sample_rate: f64,
        block_size: u32,
        input_channels: u32,
        output_channels: u32,
        verbosity: u32,
        guest_memory: *mut c_void,
        guest_memory_bytes: u32,
        guest_config: *const c_void,
        guest_config_bytes: u32,
        arena_bytes: u32,
        assets: *const c_void,
        assets_bytes: u32,
        returns: *mut c_void,
        returns_bytes: u32,
    );

    // ── Setup ── what a host says of a core before the build it applies to.
    pub fn clockwork_set_channel_ceilings(core: *mut ClockworkCore, input_channels: u32, output_channels: u32);
    pub fn clockwork_declare_fp_env(core: *mut ClockworkCore, env: u32);
    /// A floor under the guest's bulk memory (the span its allocations come from) for the next build; 0 = the
    /// profile's default.
    pub fn clockwork_set_guest_bulk_bytes(core: *mut ClockworkCore, bytes: usize);
    /// Pins the guest's random numbers for an offline render; 0 lets it seed as it likes.
    pub fn clockwork_set_deterministic_seed(core: *mut ClockworkCore, seed: i32);
    /// Why the last build has no guest, or null when it has one. NUL-terminated.
    pub fn clockwork_boot_error(core: *mut ClockworkCore) -> *const c_char;

    // ── Forwarding ── the host answers the subsystem verbs itself.
    pub fn clockwork_host_forward(core: *mut ClockworkCore, on: c_int);
    pub fn clockwork_host_forwards(core: *mut ClockworkCore) -> c_int;

    // ── Reserved lanes ──
    pub fn clockwork_reserve_lanes(core: *mut ClockworkCore, lanes: u32);
    pub fn clockwork_reserved_lanes(core: *mut ClockworkCore) -> u32;
    pub fn clockwork_lane_base(core: *mut ClockworkCore) -> u32;

    // ── Layout ── the arena's own table (`arena`), and the base it sits at.
    pub fn clockwork_arena_header(core: *mut ClockworkCore) -> *const crate::arena::ClockworkArenaHeader;
    pub fn clockwork_lanes_base(core: *mut ClockworkCore) -> *mut c_void;
    pub fn clockwork_channel_map(core: *mut ClockworkCore) -> *const c_void;
}
