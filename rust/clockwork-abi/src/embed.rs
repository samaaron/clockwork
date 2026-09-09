// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! An engine in your process (`src/clockwork_embed.h`): boot it on the
//! device, or attach and render it yourself, and get a client either way.

use core::ffi::c_char;

use crate::client::{ClockworkClient, ClockworkStatus};

/// The handle. Opaque; one per engine.
#[repr(C)]
pub struct ClockworkEmbed {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkEmbedConfig {
    /// `size_of::<ClockworkEmbedConfig>()`, for later fields.
    pub struct_bytes: u32,
    /// boot: 0 takes the device's; attach: required.
    pub sample_rate: f64,
    /// Frames per engine block; 0 = the platform default.
    pub block_size: u32,
    /// Ceilings. boot: 0 = what the device has.
    pub input_channels: u32,
    pub output_channels: u32,
    /// attach: the largest `frames` you will pass to render; 0 = 4096.
    pub max_render_frames: u32,
    /// boot: the name the OS shows; null = the product's.
    pub app_name: *const c_char,
    /// boot: a device to open by name; null = the default.
    pub device: *const c_char,
    /// 0. Reserved for a process with more than one engine.
    pub instance_id: u32,
    /// [`CLOCKWORK_EMBED_NO_INPUT`], or 0.
    pub flags: u32,
    /// boot: the driver (device type) to open on, by the name the platform
    /// reports — "Windows Audio", "DirectSound", "ASIO", "CoreAudio",
    /// "PipeWire", "JACK", "ALSA"; null = the platform's default. Resolved
    /// as a host's `--audio-driver` is: exact, else a unique
    /// case-insensitive match; an unresolvable name keeps the default with
    /// a warning. ASIO needs `device` too.
    pub driver: *const c_char,
}

/// What a booted engine is running on — the driver and device that opened
/// (not necessarily what was asked for), rate, hardware callback, block,
/// channels, latencies. Fixed-size strings: nothing here has a lifetime.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClockworkEmbedDevice {
    /// `size_of::<ClockworkEmbedDevice>()`, for later fields.
    pub struct_bytes: u32,
    pub driver: [c_char; 64],
    pub device: [c_char; 256],
    pub sample_rate: f64,
    /// The hardware callback, in frames.
    pub buffer_frames: u32,
    /// The engine's block, as `clockwork_embed_block_size`.
    pub block_frames: u32,
    pub input_channels: u32,
    pub output_channels: u32,
    pub output_latency_frames: u32,
    pub input_latency_frames: u32,
}

impl ClockworkEmbedDevice {
    /// Zeroed but for `struct_bytes`, ready to be filled.
    pub const fn new() -> Self {
        Self {
            struct_bytes: core::mem::size_of::<Self>() as u32,
            driver: [0; 64],
            device: [0; 256],
            sample_rate: 0.0,
            buffer_frames: 0,
            block_frames: 0,
            input_channels: 0,
            output_channels: 0,
            output_latency_frames: 0,
            input_latency_frames: 0,
        }
    }
    /// The driver's name, as far as it is valid UTF-8.
    pub fn driver_str(&self) -> &str {
        Self::field_str(&self.driver)
    }
    /// The device's name, as far as it is valid UTF-8.
    pub fn device_str(&self) -> &str {
        Self::field_str(&self.device)
    }
    fn field_str(f: &[c_char]) -> &str {
        // SAFETY: `f` is a live array of `c_char`, which has `u8`'s size and
        // alignment, and the view is exactly its length; nothing is written.
        let bytes: &[u8] = unsafe { core::slice::from_raw_parts(f.as_ptr().cast::<u8>(), f.len()) };
        let n = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
        core::str::from_utf8(&bytes[..n]).unwrap_or("")
    }
}

impl Default for ClockworkEmbedDevice {
    fn default() -> Self {
        Self::new()
    }
}

/// boot: open the device for output only. `input_channels` cannot say this —
/// its 0 means "what the device has" — and a host that never reads input
/// should not open it: a microphone permission prompt on macOS, a second
/// stream on its own clock on DirectSound.
pub const CLOCKWORK_EMBED_NO_INPUT: u32 = 1;

impl ClockworkEmbedConfig {
    /// A config with every field zero or null but `struct_bytes`.
    pub const fn new() -> Self {
        Self {
            struct_bytes: core::mem::size_of::<Self>() as u32,
            sample_rate: 0.0,
            block_size: 0,
            input_channels: 0,
            output_channels: 0,
            max_render_frames: 0,
            app_name: core::ptr::null(),
            device: core::ptr::null(),
            instance_id: 0,
            flags: 0,
            driver: core::ptr::null(),
        }
    }
}

impl Default for ClockworkEmbedConfig {
    fn default() -> Self { Self::new() }
}

unsafe extern "C" {
    pub fn clockwork_embed_boot(config: *const ClockworkEmbedConfig, status: *mut ClockworkStatus) -> *mut ClockworkEmbed;
    pub fn clockwork_embed_attach(config: *const ClockworkEmbedConfig, status: *mut ClockworkStatus) -> *mut ClockworkEmbed;
    pub fn clockwork_embed_client(h: *mut ClockworkEmbed) -> *mut ClockworkClient;
    pub fn clockwork_embed_sample_rate(h: *const ClockworkEmbed) -> f64;
    pub fn clockwork_embed_block_size(h: *const ClockworkEmbed) -> u32;
    pub fn clockwork_embed_device(h: *const ClockworkEmbed, out: *mut ClockworkEmbedDevice) -> ClockworkStatus;
    pub fn clockwork_embed_render(
        h: *mut ClockworkEmbed,
        out: *const *mut f32, out_channels: u32,
        input: *const *const f32, in_channels: u32,
        frames: u32,
    ) -> u32;
    pub fn clockwork_embed_close(h: *mut ClockworkEmbed);
}
