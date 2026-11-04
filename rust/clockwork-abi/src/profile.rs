// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The sizes a core's arena is laid out from (`src/clockwork_profile.h`,
//! docs/ENGINES.md): the three rings, the scope, the taps, the
//! guest's window and persistent bytes. Clients need none of this: they
//! read every region from the arena's table.

use core::ffi::c_char;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClockworkProfile {
    /// `size_of::<ClockworkProfile>()`.
    pub struct_bytes: u32,
    /// Messages to the engine; bulk payloads ride it.
    pub in_ring_bytes: u32,
    /// The audio thread's replies.
    pub rt_out_ring_bytes: u32,
    /// Every other thread's replies, notifications and log.
    pub nrt_out_ring_bytes: u32,
    /// Per scope slot, out and in.
    pub scope_frames: u32,
    /// The widest a scope slot carries.
    pub scope_channels: u32,
    /// The guest's tap streams.
    pub tap_slots: u32,
    /// The engine's, numbered after the guest's.
    pub track_tap_slots: u32,
    /// Per tap stream.
    pub tap_ring_frames: u32,
    /// The guest's window.
    pub window_bytes: u32,
    /// The guest's persistent bytes.
    pub persist_bytes: u32,
}

impl ClockworkProfile {
    /// Every size zero but `struct_bytes`: to be filled by
    /// `clockwork_profile_default` or `clockwork_profile_plugin`.
    pub const fn new() -> Self {
        Self {
            struct_bytes: core::mem::size_of::<Self>() as u32,
            in_ring_bytes: 0,
            rt_out_ring_bytes: 0,
            nrt_out_ring_bytes: 0,
            scope_frames: 0,
            scope_channels: 0,
            tap_slots: 0,
            track_tap_slots: 0,
            tap_ring_frames: 0,
            window_bytes: 0,
            persist_bytes: 0,
        }
    }
}

impl Default for ClockworkProfile {
    fn default() -> Self { Self::new() }
}

extern "C" {
    pub fn clockwork_profile_default(out: *mut ClockworkProfile);
    pub fn clockwork_profile_plugin(out: *mut ClockworkProfile);
    pub fn clockwork_profile_check(profile: *const ClockworkProfile) -> *const c_char;
}
