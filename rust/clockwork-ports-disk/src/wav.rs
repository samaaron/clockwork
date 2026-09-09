// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! WAV, parsed and written by hand.
//!
//! No codec crate, and that is a deliberate cost. This repository vendors no
//! decoding stack, and the claim that clockwork contains no DSP and pulls in
//! no media libraries is worth more than the two hundred lines saved — a codec
//! dependency arrives with its own licence, its own transitive tree and its own
//! opinions about resampling. What is needed here is a chunk walk and two
//! sample conversions.
//!
//! Supported: 16-bit integer PCM (`WAVE_FORMAT_PCM`) and 32-bit IEEE float
//! (`WAVE_FORMAT_IEEE_FLOAT`), plus `WAVE_FORMAT_EXTENSIBLE` whose subformat
//! GUID begins with either of those. Anything else is refused by name rather
//! than played as noise.
//!
//! Written files are 32-bit float, so a round trip through a sink and back
//! through a source is bit-exact and a test can assert equality rather than a
//! tolerance.

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::Path;

pub const FORMAT_PCM: u16 = 1;
pub const FORMAT_FLOAT: u16 = 3;
const FORMAT_EXTENSIBLE: u16 = 0xFFFE;

fn u16le(b: &[u8]) -> u16 { u16::from_le_bytes([b[0], b[1]]) }
fn u32le(b: &[u8]) -> u32 { u32::from_le_bytes([b[0], b[1], b[2], b[3]]) }

fn bad(what: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, what.to_string())
}

// ── Reading ─────────────────────────────────────────────────────────────────

pub struct WavReader {
    file: File,
    pub channels: u16,
    pub sample_rate: u32,
    /// Bits per sample as the file declares them.
    pub bits: u16,
    /// Resolved format: `FORMAT_PCM` or `FORMAT_FLOAT`, extensible unwrapped.
    pub format: u16,
    pub total_frames: u64,
    frames_read: u64,
    bytes_per_frame: usize,
}

impl WavReader {
    pub fn open(path: &Path) -> io::Result<WavReader> {
        let mut file = File::open(path)?;

        let mut riff = [0u8; 12];
        file.read_exact(&mut riff)?;
        if &riff[0..4] != b"RIFF" || &riff[8..12] != b"WAVE" {
            return Err(bad("not a RIFF/WAVE file"));
        }

        let mut channels = 0u16;
        let mut sample_rate = 0u32;
        let mut bits = 0u16;
        let mut format = 0u16;
        let mut data_len: u64 = 0;
        let mut have_fmt = false;

        // The chunk walk. Chunks are 2-aligned and a chunk of odd size is
        // followed by one pad byte that is NOT counted in its size — get that
        // wrong and every chunk after the first odd one is garbage.
        loop {
            let mut hdr = [0u8; 8];
            match file.read_exact(&mut hdr) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
                Err(e) => return Err(e),
            }
            let id = [hdr[0], hdr[1], hdr[2], hdr[3]];
            let size = u32le(&hdr[4..8]) as u64;

            if &id == b"fmt " {
                if size < 16 { return Err(bad("fmt chunk too short")); }
                let mut fmt = vec![0u8; size as usize];
                file.read_exact(&mut fmt)?;
                format = u16le(&fmt[0..2]);
                channels = u16le(&fmt[2..4]);
                sample_rate = u32le(&fmt[4..8]);
                bits = u16le(&fmt[14..16]);
                if format == FORMAT_EXTENSIBLE {
                    // The real format is the first two bytes of the subformat
                    // GUID, 24 bytes into the extension.
                    if size < 40 { return Err(bad("extensible fmt chunk too short")); }
                    format = u16le(&fmt[24..26]);
                }
                have_fmt = true;
            } else if &id == b"data" {
                data_len = size;
                // Everything after this is samples; stop walking.
                break;
            } else {
                file.seek(SeekFrom::Current(size as i64 + (size & 1) as i64))?;
                continue;
            }
            if size & 1 == 1 { file.seek(SeekFrom::Current(1))?; }
        }

        if !have_fmt { return Err(bad("no fmt chunk")); }
        if channels == 0 { return Err(bad("zero channels")); }
        let bytes_per_sample = match (format, bits) {
            (FORMAT_PCM, 16) => 2usize,
            (FORMAT_FLOAT, 32) => 4usize,
            (f, b) => {
                return Err(bad(&format!(
                    "unsupported WAV format {} at {} bits — this reader does \
                     16-bit PCM and 32-bit float only", f, b)))
            }
        };
        let bytes_per_frame = bytes_per_sample * channels as usize;
        let total_frames = data_len / bytes_per_frame as u64;

        Ok(WavReader {
            file, channels, sample_rate, bits, format,
            total_frames, frames_read: 0, bytes_per_frame,
        })
    }

    pub fn frames_remaining(&self) -> u64 { self.total_frames - self.frames_read }

    /// Read up to `frames` interleaved frames as f32 into `out`, which must
    /// hold `frames * channels` samples. Returns frames actually read.
    pub fn read_frames(&mut self, out: &mut [f32], frames: usize) -> io::Result<usize> {
        let want = core::cmp::min(frames as u64, self.frames_remaining()) as usize;
        if want == 0 { return Ok(0); }
        let ch = self.channels as usize;

        let mut raw = vec![0u8; want * self.bytes_per_frame];
        self.file.read_exact(&mut raw)?;
        self.frames_read += want as u64;

        match self.format {
            FORMAT_PCM => {
                for i in 0..want * ch {
                    let v = i16::from_le_bytes([raw[i * 2], raw[i * 2 + 1]]);
                    // 32768 rather than 32767: it makes the mapping exact for
                    // every value and puts full-scale negative at -1.0, which
                    // is the convention every other tool here uses.
                    out[i] = v as f32 / 32768.0;
                }
            }
            _ => {
                for i in 0..want * ch {
                    out[i] = f32::from_le_bytes([
                        raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], raw[i * 4 + 3],
                    ]);
                }
            }
        }
        Ok(want)
    }
}

// ── Writing ─────────────────────────────────────────────────────────────────

/// Byte offsets of the three fields patched on close. Named rather than
/// counted at the call site, because an off-by-four here writes a plausible
/// header describing the wrong length.
const OFF_RIFF_SIZE: u64 = 4;
const OFF_FACT_FRAMES: u64 = 46;
const OFF_DATA_SIZE: u64 = 54;
const HEADER_BYTES: u64 = 58;

pub struct WavWriter {
    file: File,
    channels: u16,
    frames: u64,
}

impl WavWriter {
    pub fn create(path: &Path, sample_rate: u32, channels: u16) -> io::Result<WavWriter> {
        if channels == 0 { return Err(bad("zero channels")); }
        let mut file = File::create(path)?;
        let block_align = 4u16 * channels;
        let byte_rate = sample_rate * block_align as u32;

        let mut h: Vec<u8> = Vec::with_capacity(HEADER_BYTES as usize);
        h.extend_from_slice(b"RIFF");
        h.extend_from_slice(&0u32.to_le_bytes());       // patched
        h.extend_from_slice(b"WAVE");
        // fmt: size 18, not 16 — a non-PCM format is required to carry cbSize,
        // and a reader that trusts the size (this one does) is happy either
        // way, while a stricter one is not.
        h.extend_from_slice(b"fmt ");
        h.extend_from_slice(&18u32.to_le_bytes());
        h.extend_from_slice(&FORMAT_FLOAT.to_le_bytes());
        h.extend_from_slice(&channels.to_le_bytes());
        h.extend_from_slice(&sample_rate.to_le_bytes());
        h.extend_from_slice(&byte_rate.to_le_bytes());
        h.extend_from_slice(&block_align.to_le_bytes());
        h.extend_from_slice(&32u16.to_le_bytes());
        h.extend_from_slice(&0u16.to_le_bytes());       // cbSize
        h.extend_from_slice(b"fact");
        h.extend_from_slice(&4u32.to_le_bytes());
        h.extend_from_slice(&0u32.to_le_bytes());       // patched
        h.extend_from_slice(b"data");
        h.extend_from_slice(&0u32.to_le_bytes());       // patched
        debug_assert_eq!(h.len() as u64, HEADER_BYTES);
        file.write_all(&h)?;

        Ok(WavWriter { file, channels, frames: 0 })
    }

    pub fn frames(&self) -> u64 { self.frames }

    /// Append interleaved f32 frames.
    pub fn write_frames(&mut self, data: &[f32], frames: usize) -> io::Result<()> {
        let n = frames * self.channels as usize;
        let mut raw = Vec::with_capacity(n * 4);
        for &v in &data[..n] { raw.extend_from_slice(&v.to_le_bytes()); }
        self.file.write_all(&raw)?;
        self.frames += frames as u64;
        Ok(())
    }

    /// Patch the three lengths and flush. A file that was never closed is
    /// still readable by anything that walks chunks and clamps to the file
    /// size, but it claims zero frames — so this is not optional.
    pub fn finish(&mut self) -> io::Result<()> {
        let data_bytes = self.frames * 4 * self.channels as u64;
        let riff = HEADER_BYTES - 8 + data_bytes;
        self.file.flush()?;
        self.file.seek(SeekFrom::Start(OFF_RIFF_SIZE))?;
        self.file.write_all(&(riff as u32).to_le_bytes())?;
        self.file.seek(SeekFrom::Start(OFF_FACT_FRAMES))?;
        self.file.write_all(&(self.frames as u32).to_le_bytes())?;
        self.file.seek(SeekFrom::Start(OFF_DATA_SIZE))?;
        self.file.write_all(&(data_bytes as u32).to_le_bytes())?;
        self.file.flush()
    }
}
