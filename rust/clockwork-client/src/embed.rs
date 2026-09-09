// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! An engine in your process (`clockwork_embed.h`).
//!
//! Two ways to bring one up and the same handle either way: [`Embed::boot`]
//! opens the audio device and ticks itself; [`Embed::attach`] opens nothing
//! and you render. Above the boot line they are identical — the engine's
//! [`Client`] is [`Embed::client`] — so a product that plays the same piece
//! as an app and as a plugin writes its playing once.

use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;
use std::ffi::CString;

use clockwork_abi::client::ClockworkStatus;
use clockwork_abi::embed::{
    clockwork_embed_attach, clockwork_embed_block_size, clockwork_embed_boot,
    clockwork_embed_client, clockwork_embed_close, clockwork_embed_device,
    clockwork_embed_render, clockwork_embed_sample_rate, ClockworkEmbed, ClockworkEmbedConfig,
    ClockworkEmbedDevice, CLOCKWORK_EMBED_NO_INPUT,
};

use crate::client::Client;
use crate::error::{check, refused, Error, Result};

/// The most channels one render call carries either way.
pub const RENDER_MAX_CHANNELS: usize = 64;

/// How to bring an engine up. Every field is optional for a boot; an attach
/// needs a sample rate and output channels.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EmbedConfig {
    sample_rate: f64,
    block_size: u32,
    input_channels: u32,
    output_channels: u32,
    max_render_frames: u32,
    app_name: Option<String>,
    device: Option<String>,
    driver: Option<String>,
    no_input: bool,
}

impl EmbedConfig {
    /// The platform's defaults for everything.
    pub fn new() -> EmbedConfig {
        EmbedConfig::default()
    }

    /// boot: 0 takes the device's. attach: required.
    pub fn sample_rate(mut self, hz: f64) -> Self {
        self.sample_rate = hz;
        self
    }

    /// Frames per engine block; 0 is the platform default.
    pub fn block_size(mut self, frames: u32) -> Self {
        self.block_size = frames;
        self
    }

    /// A ceiling. boot: 0 is what the device has; see [`Self::no_input`]
    /// for none. attach: what you will hand to render, at most.
    pub fn input_channels(mut self, channels: u32) -> Self {
        self.input_channels = channels;
        self
    }

    /// A ceiling. boot: 0 is what the device has. attach: what you will
    /// render, at most — required.
    pub fn output_channels(mut self, channels: u32) -> Self {
        self.output_channels = channels;
        self
    }

    /// attach: the largest `frames` one render will ask for; 0 is 4096.
    pub fn max_render_frames(mut self, frames: u32) -> Self {
        self.max_render_frames = frames;
        self
    }

    /// boot: the name the OS shows; the product's by default.
    pub fn app_name(mut self, name: &str) -> Self {
        self.app_name = Some(name.to_owned());
        self
    }

    /// boot: a device to open by name, matched the way the engine matches
    /// any requested device; the default otherwise.
    pub fn device(mut self, name: &str) -> Self {
        self.device = Some(name.to_owned());
        self
    }

    /// boot: the driver (device type) to open on — the names the platform
    /// reports: "Windows Audio", "DirectSound", "ASIO", "CoreAudio",
    /// "PipeWire", "JACK", "ALSA". The exact name, else a unique
    /// case-insensitive match; a name that resolves to nothing keeps the
    /// default with a warning rather than refusing to boot. ASIO needs a
    /// device as well.
    pub fn driver(mut self, name: &str) -> Self {
        self.driver = Some(name.to_owned());
        self
    }

    /// boot: open the device for output only. A host that never reads
    /// input should not open it: on macOS that is a microphone permission
    /// prompt, and on DirectSound a second stream on its own clock.
    pub fn no_input(mut self, yes: bool) -> Self {
        self.no_input = yes;
        self
    }

    /// The ABI's struct, with the strings it points at held beside it.
    fn raw(&self) -> Result<RawConfig> {
        let c = |s: &Option<String>| -> Result<Option<CString>> {
            match s {
                Some(s) => CString::new(s.as_str()).map(Some).map_err(|_| Error::Arg),
                None => Ok(None),
            }
        };
        let strings = [c(&self.app_name)?, c(&self.device)?, c(&self.driver)?];
        let ptr = |s: &Option<CString>| s.as_ref().map_or(core::ptr::null(), |s| s.as_ptr());
        let mut cfg = ClockworkEmbedConfig::new();
        cfg.sample_rate = self.sample_rate;
        cfg.block_size = self.block_size;
        cfg.input_channels = self.input_channels;
        cfg.output_channels = self.output_channels;
        cfg.max_render_frames = self.max_render_frames;
        cfg.app_name = ptr(&strings[0]);
        cfg.device = ptr(&strings[1]);
        cfg.driver = ptr(&strings[2]);
        cfg.flags = if self.no_input { CLOCKWORK_EMBED_NO_INPUT } else { 0 };
        Ok(RawConfig { cfg, _strings: strings })
    }
}

/// The ABI struct and the heap strings its pointers name. A `CString`'s
/// bytes do not move when the `CString` does, so this may be returned and
/// moved before the call that reads it.
struct RawConfig {
    cfg: ClockworkEmbedConfig,
    _strings: [Option<CString>; 3],
}

/// What a booted engine is actually running on, which may not be what was
/// asked for.
#[derive(Clone, Debug, PartialEq)]
pub struct Device {
    /// "Windows Audio", "CoreAudio", ...
    pub driver: String,
    /// The output device's name.
    pub device: String,
    pub sample_rate: f64,
    /// The hardware callback, in frames.
    pub buffer_frames: u32,
    /// The engine's block, as [`Embed::block_size`].
    pub block_frames: u32,
    pub input_channels: u32,
    pub output_channels: u32,
    pub output_latency_frames: u32,
    pub input_latency_frames: u32,
}

impl Device {
    fn from_raw(d: &ClockworkEmbedDevice) -> Device {
        Device {
            driver: d.driver_str().to_owned(),
            device: d.device_str().to_owned(),
            sample_rate: d.sample_rate,
            buffer_frames: d.buffer_frames,
            block_frames: d.block_frames,
            input_channels: d.input_channels,
            output_channels: d.output_channels,
            output_latency_frames: d.output_latency_frames,
            input_latency_frames: d.input_latency_frames,
        }
    }
}

/// An engine in this process, and the client over it. Closing the handle
/// closes the engine — a booted one closes its device and shuts down, an
/// attached one detaches — and the client with it.
///
/// One engine per process today: a second boot or attach is refused with
/// [`Error::Perm`].
///
/// Derefs to its [`Client`], so `embed.send(..)` and `embed.poll()` are the
/// client's. [`Embed::split`] hands the render half to another thread.
pub struct Embed {
    raw: NonNull<ClockworkEmbed>,
    client: Client,
}

// SAFETY: as for Client — nothing is pinned to the opening thread, and the
// handle is used from one thread at a time; render alone may run elsewhere,
// through a `Renderer` that carries no client half.
unsafe impl Send for Embed {}

impl Embed {
    /// Bring an engine up on the audio device and let it tick itself.
    /// [`Error::Absent`] when this build has no device layer, [`Error::Perm`]
    /// when the process already has an engine.
    pub fn boot(config: &EmbedConfig) -> Result<Embed> {
        let raw = config.raw()?;
        let mut st = ClockworkStatus::OK;
        // SAFETY: a struct that says its own size, whose strings live in
        // `raw` across the call; a live out-pointer.
        let h = unsafe { clockwork_embed_boot(&raw.cfg, &mut st) };
        Self::opened(h, st)
    }

    /// Bring an engine up with no device: you render. Needs a sample rate
    /// and output channels; [`Error::Perm`] when the process already has an
    /// engine.
    pub fn attach(config: &EmbedConfig) -> Result<Embed> {
        let raw = config.raw()?;
        let mut st = ClockworkStatus::OK;
        // SAFETY: as `boot`.
        let h = unsafe { clockwork_embed_attach(&raw.cfg, &mut st) };
        Self::opened(h, st)
    }

    fn opened(h: *mut ClockworkEmbed, st: ClockworkStatus) -> Result<Embed> {
        let raw = NonNull::new(h).ok_or_else(|| refused(st))?;
        // SAFETY: a live handle. Its client is owned by it and closed with
        // it, which is what `owned = false` below records.
        let client = unsafe { clockwork_embed_client(raw.as_ptr()) };
        match Client::opened(client, ClockworkStatus::E_CLOSED, false) {
            Ok(client) => Ok(Embed { raw, client }),
            Err(e) => {
                // SAFETY: ours, opened just above, closed once here.
                unsafe { clockwork_embed_close(raw.as_ptr()) };
                Err(e)
            }
        }
    }

    /// The client over this engine.
    pub fn client(&self) -> &Client {
        &self.client
    }

    /// The client over this engine, for polling.
    pub fn client_mut(&mut self) -> &mut Client {
        &mut self.client
    }

    /// The two halves at once: a [`Renderer`] to hand to the audio thread
    /// and the client to keep. Borrows the handle for as long as either
    /// lives.
    pub fn split(&mut self) -> (Renderer<'_>, &mut Client) {
        (Renderer { raw: self.raw, _embed: PhantomData }, &mut self.client)
    }

    /// The rate the engine actually runs at (an attach may have joined one).
    pub fn sample_rate(&self) -> f64 {
        // SAFETY: a live handle.
        unsafe { clockwork_embed_sample_rate(self.raw.as_ptr()) }
    }

    /// The engine's block, in frames.
    pub fn block_size(&self) -> u32 {
        // SAFETY: a live handle.
        unsafe { clockwork_embed_block_size(self.raw.as_ptr()) }
    }

    /// What a booted engine is running on. [`Error::Arg`] for an attached
    /// one — it has no device; the host renders.
    pub fn device(&self) -> Result<Device> {
        let mut d = ClockworkEmbedDevice::new();
        // SAFETY: a live handle and a struct that says its own size.
        check(unsafe { clockwork_embed_device(self.raw.as_ptr(), &mut d) })?;
        Ok(Device::from_raw(&d))
    }

    /// Render `frames` frames: each `out` channel's first `frames` are
    /// filled, each `input` channel's first `frames` consumed. See
    /// [`Renderer::render`].
    pub fn render(&mut self, out: &mut [&mut [f32]], input: &[&[f32]], frames: usize) -> usize {
        render(self.raw, out, input, frames)
    }

    /// The raw handle, for a call this crate does not wrap. Owned by this
    /// wrapper: do not close it.
    pub fn as_ptr(&self) -> *mut ClockworkEmbed {
        self.raw.as_ptr()
    }
}

impl Deref for Embed {
    type Target = Client;
    fn deref(&self) -> &Client {
        &self.client
    }
}

impl DerefMut for Embed {
    fn deref_mut(&mut self) -> &mut Client {
        &mut self.client
    }
}

impl Drop for Embed {
    fn drop(&mut self) {
        // The client is the handle's and closes with it; its own Drop runs
        // after this and, being unowned, does nothing.
        // SAFETY: ours to close, closed once.
        unsafe { clockwork_embed_close(self.raw.as_ptr()) }
    }
}

/// The render half of an attached engine, for the audio thread. Carries no
/// client, so nothing a client thread does can race it; [`Send`], so a
/// process callback on another thread can own it for the handle's life.
pub struct Renderer<'e> {
    raw: NonNull<ClockworkEmbed>,
    _embed: PhantomData<&'e Embed>,
}

// SAFETY: render is audio-thread safe by contract (clockwork_embed.h) — no
// allocation, no lock — and its FIFOs are touched by nothing else on the
// handle, so the one thread holding this may call it beside a client thread.
unsafe impl Send for Renderer<'_> {}

impl Renderer<'_> {
    /// Render `frames` frames: `out[c][..frames]` is filled for every output
    /// channel, `input[c][..frames]` consumed for every input channel.
    /// Channels past the configured ceilings are silence out and ignored
    /// in. Any `frames` is answered with exactly `frames` — the engine
    /// renders whole blocks behind two small FIFOs — and input reaches it
    /// up to one block late. Audio-thread safe. Returns the frames rendered:
    /// `frames`, or 0 for a booted engine, which renders itself.
    ///
    /// Panics if a slice is shorter than `frames` or there are more than
    /// [`RENDER_MAX_CHANNELS`] either way.
    pub fn render(&mut self, out: &mut [&mut [f32]], input: &[&[f32]], frames: usize) -> usize {
        render(self.raw, out, input, frames)
    }
}

fn render(raw: NonNull<ClockworkEmbed>, out: &mut [&mut [f32]], input: &[&[f32]], frames: usize) -> usize {
    assert!(out.len() <= RENDER_MAX_CHANNELS, "more than RENDER_MAX_CHANNELS output channels");
    assert!(input.len() <= RENDER_MAX_CHANNELS, "more than RENDER_MAX_CHANNELS input channels");
    let frames_u32 = u32::try_from(frames).expect("frames fits a u32");
    let mut outp = [core::ptr::null_mut::<f32>(); RENDER_MAX_CHANNELS];
    let mut inp = [core::ptr::null::<f32>(); RENDER_MAX_CHANNELS];
    for (p, ch) in outp.iter_mut().zip(out.iter_mut()) {
        assert!(ch.len() >= frames, "an output channel is shorter than `frames`");
        *p = ch.as_mut_ptr();
    }
    for (p, ch) in inp.iter_mut().zip(input.iter()) {
        assert!(ch.len() >= frames, "an input channel is shorter than `frames`");
        *p = ch.as_ptr();
    }
    // SAFETY: a live handle; every pointer names a slice of at least
    // `frames` floats that lives across the call, and the counts passed are
    // exactly how many pointers were filled.
    let n = unsafe {
        clockwork_embed_render(
            raw.as_ptr(),
            outp.as_ptr(),
            out.len() as u32,
            inp.as_ptr(),
            input.len() as u32,
            frames_u32,
        )
    };
    n as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_config_is_the_header_s_struct_with_its_strings_alive() {
        let cfg = EmbedConfig::new()
            .sample_rate(48_000.0)
            .block_size(128)
            .input_channels(1)
            .output_channels(2)
            .max_render_frames(512)
            .app_name("test")
            .device("Speakers")
            .driver("Windows Audio")
            .no_input(true);
        let raw = cfg.raw().unwrap();
        assert_eq!(raw.cfg.struct_bytes as usize, core::mem::size_of::<ClockworkEmbedConfig>());
        assert_eq!(raw.cfg.sample_rate, 48_000.0);
        assert_eq!(raw.cfg.block_size, 128);
        assert_eq!((raw.cfg.input_channels, raw.cfg.output_channels), (1, 2));
        assert_eq!(raw.cfg.max_render_frames, 512);
        assert_eq!(raw.cfg.flags, CLOCKWORK_EMBED_NO_INPUT);
        // SAFETY: the pointers name NUL-terminated strings held in `raw`.
        let s = |p: *const core::ffi::c_char| unsafe { core::ffi::CStr::from_ptr(p) }.to_str().unwrap();
        assert_eq!(s(raw.cfg.app_name), "test");
        assert_eq!(s(raw.cfg.device), "Speakers");
        assert_eq!(s(raw.cfg.driver), "Windows Audio");

        let bare = EmbedConfig::new().raw().unwrap();
        assert!(bare.cfg.app_name.is_null() && bare.cfg.device.is_null() && bare.cfg.driver.is_null());
        assert_eq!(bare.cfg.flags, 0);

        // A NUL inside a name cannot be a C string; refused here, not truncated.
        assert_eq!(EmbedConfig::new().driver("bad\0name").raw().err(), Some(Error::Arg));
    }

    #[test]
    fn a_device_read_back_is_copied_out_of_the_fixed_size_strings() {
        let mut d = ClockworkEmbedDevice::new();
        d.driver[..5].copy_from_slice(b"ALSA\0".map(|b| b as core::ffi::c_char).as_slice());
        d.device[..3].copy_from_slice(b"hw\0".map(|b| b as core::ffi::c_char).as_slice());
        d.sample_rate = 44_100.0;
        d.buffer_frames = 256;
        let dev = Device::from_raw(&d);
        assert_eq!(dev.driver, "ALSA");
        assert_eq!(dev.device, "hw");
        assert_eq!(dev.sample_rate, 44_100.0);
        assert_eq!(dev.buffer_frames, 256);
    }
}
