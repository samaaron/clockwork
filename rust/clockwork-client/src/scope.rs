// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A scope slot: a ring of audio the engine keeps filling.
//!
//! Reading one is three steps wherever it is done — find the slot, ask how
//! far the audible edge has reached, copy a window back from it — and they
//! are here, over a reader the C library fills.

use clockwork_abi::client::{
    clockwork_client_scope_audible_end, clockwork_client_scope_open, clockwork_client_scope_read,
    clockwork_client_scope_valid, ClockworkScopeReader,
};

use crate::client::Client;
use crate::error::{check, Result};

/// The most channels a scope slot carries — `SHM_SCOPE_STREAM_CHANNELS` in
/// `src/shm_scope_stream.hpp`, which the engine clamps every slot's channel
/// count to. A read buffer is sized by this, because the count a slot
/// actually has is re-read as the copy happens and may differ from one read
/// to the next. The crate's engine tests hold it to the arena's geometry.
pub const SCOPE_MAX_CHANNELS: usize = 2;

/// What one read copied: the frames that were there, and the channels the
/// slot had while they were copied.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScopeRead {
    /// Frames of real audio in the window. Less than asked for means the
    /// ring did not reach that far back; the missing front is silence.
    pub frames: usize,
    /// The slot's channel count, which is how `out` is interleaved.
    pub channels: usize,
}

/// A reader bound to one slot. Which slot carries what is between the
/// engine's embedder and its guest; clockwork numbers them and does not
/// name them.
pub struct Scope<'c> {
    reader: ClockworkScopeReader,
    client: &'c Client,
}

impl<'c> Scope<'c> {
    pub(crate) fn open(client: &'c Client, slot: u32) -> Result<Scope<'c>> {
        let mut reader = ClockworkScopeReader::for_slot(slot);
        // SAFETY: a live handle and a reader sized by its own struct_bytes.
        check(unsafe { clockwork_client_scope_open(client.as_ptr(), slot, &mut reader) })?;
        Ok(Scope { reader, client })
    }

    /// The slot this reader is bound to.
    pub fn slot(&self) -> u32 {
        self.reader.slot
    }

    /// Is the slot live? A slot deactivates across a device switch and
    /// comes back; a reader that keeps drawing through that reads silence
    /// rather than stale audio.
    pub fn is_valid(&self) -> bool {
        // SAFETY: the reader was filled by scope_open on a live handle.
        unsafe { clockwork_client_scope_valid(&self.reader) != 0 }
    }

    /// How far the audible edge has reached, in frames since the engine
    /// started. Unchanged between calls means nothing new has been
    /// rendered, which is the cheap way to skip a redraw.
    pub fn audible_end(&self) -> u64 {
        // SAFETY: as `is_valid`, on the handle the reader was opened from.
        unsafe { clockwork_client_scope_audible_end(self.client.as_ptr(), &self.reader) }
    }

    /// Copy the `frames` frames ending at `end` into `out`, interleaved.
    ///
    /// `out` is ALWAYS fully written for the frames read: the window is
    /// right-aligned, real audio at the back and silence at the front where
    /// the ring had nothing, and a slot that went away yields all silence —
    /// so a caller never draws the previous window's audio by accident.
    /// `frames` is clamped to what `out` can hold at [`SCOPE_MAX_CHANNELS`].
    pub fn read(&self, end: u64, frames: usize, out: &mut [f32]) -> ScopeRead {
        let frames = frames.min(out.len() / SCOPE_MAX_CHANNELS);
        if frames == 0 {
            return ScopeRead::default();
        }
        let mut channels = 0u32;
        // SAFETY: `out` holds frames * SCOPE_MAX_CHANNELS floats and the
        // engine writes at most frames * channels with channels clamped to
        // SCOPE_MAX_CHANNELS (shm_scope_stream.hpp); the reader was filled
        // by scope_open on this handle, and the out-pointer is live.
        let got = unsafe {
            clockwork_client_scope_read(
                self.client.as_ptr(),
                &self.reader,
                end,
                frames as u32,
                out.as_mut_ptr(),
                &mut channels,
            )
        };
        ScopeRead { frames: got as usize, channels: channels as usize }
    }

    /// The last `frames` frames that have been heard: [`Scope::read`] at
    /// [`Scope::audible_end`]. What a waveform display draws.
    pub fn read_audible(&self, frames: usize, out: &mut [f32]) -> ScopeRead {
        self.read(self.audible_end(), frames, out)
    }

    /// The last `frames` frames the engine has WRITTEN, which is ahead of
    /// what has been heard by the device's buffer. For a test, or a display
    /// that would rather run early than late.
    pub fn read_newest(&self, frames: usize, out: &mut [f32]) -> ScopeRead {
        self.read(u64::MAX, frames, out)
    }
}
