// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The uncompressed containers, read by hand before the decoder sees them.
//!
//! A WAV, an AIFF, an RF64 and a Wave64 all say in a header how many bytes of
//! samples follow, and a hostile one says anything. The file's own length is
//! the truth, so every one of them is walked here and handed on as a plain
//! RIFF/WAVE or FORM/AIFF whose data chunk is exactly the bytes that are
//! there. The decoder never meets the claim. RF64 and Wave64 exist for takes
//! past 4 GB, which no decode into memory can hold anyway, so presenting them
//! as RIFF loses nothing a limit would not refuse.
//!
//! Nothing is copied: the rewritten header is a small vector and the data is
//! the caller's slice, chained as one seekable source.

use std::io::{Read, Seek, SeekFrom};

use symphonia::core::io::MediaSource;

use crate::Format;

/// The first sixteen bytes of a Wave64 file: the RIFF GUID.
const W64_RIFF: [u8; 16] = [
    0x72, 0x69, 0x66, 0x66, 0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00,
];

/// What the bytes are, from their first bytes alone and never from a name.
pub fn sniff(d: &[u8]) -> Format {
    let starts = |at: usize, tag: &[u8]| d.len() >= at + tag.len() && &d[at..at + tag.len()] == tag;
    if d.len() < 4 {
        return Format::Unknown;
    }
    if starts(0, b"fLaC") {
        return Format::Flac;
    }
    if starts(0, b"OggS") {
        return Format::Ogg;
    }
    if starts(0, b"RF64") && starts(8, b"WAVE") {
        return Format::Rf64;
    }
    if d.len() >= 16 && d[..16] == W64_RIFF {
        return Format::W64;
    }
    if starts(0, b"RIFF") && starts(8, b"WAVE") {
        return Format::Wav;
    }
    if starts(0, b"FORM") && (starts(8, b"AIFF") || starts(8, b"AIFC")) {
        return Format::Aiff;
    }
    // MP3 carries no container magic. An ID3 tag, or a frame sync with a
    // layer III version that is not the reserved one, is as much as there is
    // to go on, and it is what every player uses.
    if starts(0, b"ID3") {
        return Format::Mp3;
    }
    if d[0] == 0xFF && (d[1] & 0xE0) == 0xE0 && (d[1] & 0x18) != 0x08 && (d[1] & 0x06) == 0x02 {
        return Format::Mp3;
    }
    Format::Unknown
}

/// A chunk as found: its four-character id, and where its payload lies in the
/// file, clamped to what the file holds.
struct Chunk {
    id: [u8; 4],
    start: usize,
    len: usize,
    /// The header's own claim, before the clamp.
    claimed: u64,
}

fn le32(d: &[u8], at: usize) -> Option<u32> {
    d.get(at..at + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}
fn be32(d: &[u8], at: usize) -> Option<u32> {
    d.get(at..at + 4)
        .map(|b| u32::from_be_bytes([b[0], b[1], b[2], b[3]]))
}
fn le64(d: &[u8], at: usize) -> Option<u64> {
    d.get(at..at + 8)
        .map(|b| u64::from_le_bytes([b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]]))
}

/// The chunks of a RIFF (little-endian sizes) or FORM (big-endian) file from
/// byte 12, each payload padded to an even length.
fn walk_iff(d: &[u8], big_endian: bool) -> Vec<Chunk> {
    let mut out = Vec::new();
    let mut at = 12usize;
    while at + 8 <= d.len() {
        let mut id = [0u8; 4];
        id.copy_from_slice(&d[at..at + 4]);
        let claimed = u64::from(
            if big_endian {
                be32(d, at + 4)
            } else {
                le32(d, at + 4)
            }
            .unwrap_or(0),
        );
        let start = at + 8;
        let len = (claimed.min((d.len() - start) as u64)) as usize;
        out.push(Chunk {
            id,
            start,
            len,
            claimed,
        });
        // A chunk that runs past the end is the last one, whatever it claims.
        if claimed > (d.len() - start) as u64 {
            break;
        }
        at = start + len + (len & 1);
    }
    out
}

/// The chunks of a Wave64 file from byte 40: a 16-byte GUID whose first four
/// bytes are the RIFF id, then a 64-bit size that counts the 24-byte header,
/// each padded to eight.
fn walk_w64(d: &[u8]) -> Vec<Chunk> {
    let mut out = Vec::new();
    let mut at = 40usize;
    while at + 24 <= d.len() {
        let mut id = [0u8; 4];
        id.copy_from_slice(&d[at..at + 4]);
        let size = le64(d, at + 16).unwrap_or(0);
        let claimed = size.saturating_sub(24);
        let start = at + 24;
        let len = (claimed.min((d.len() - start) as u64)) as usize;
        out.push(Chunk {
            id,
            start,
            len,
            claimed,
        });
        if claimed > (d.len() - start) as u64 {
            break;
        }
        at = start + len + ((8 - (len % 8)) % 8);
    }
    out
}

/// The header of a RIFF/WAVE or FORM/AIFF rebuilt from `chunks`, with the
/// `data_id` chunk last and its size the clamped one; every other chunk is
/// copied through unchanged, so a WAVE_FORMAT_EXTENSIBLE fmt, a fact chunk or
/// an AIFC FVER all arrive as they were.
fn rebuild<'a>(
    d: &'a [u8],
    form: &[u8; 4],
    kind: &[u8; 4],
    chunks: &[Chunk],
    data_id: &[u8; 4],
    big_endian: bool,
) -> Option<Normalised<'a>> {
    let data = chunks.iter().find(|c| &c.id == data_id)?;
    let put32 = |v: &mut Vec<u8>, x: u32| {
        v.extend_from_slice(&if big_endian {
            x.to_be_bytes()
        } else {
            x.to_le_bytes()
        })
    };

    let mut head = Vec::with_capacity(64);
    head.extend_from_slice(form);
    put32(&mut head, 0); // patched below
    head.extend_from_slice(kind);
    for c in chunks {
        if &c.id == data_id {
            continue;
        }
        head.extend_from_slice(&c.id);
        put32(&mut head, c.len as u32);
        head.extend_from_slice(&d[c.start..c.start + c.len]);
        if c.len & 1 == 1 {
            head.push(0);
        }
    }
    head.extend_from_slice(data_id);
    put32(&mut head, data.len as u32);
    let total = (head.len() - 8 + data.len) as u32;
    let size = if big_endian {
        total.to_be_bytes()
    } else {
        total.to_le_bytes()
    };
    head[4..8].copy_from_slice(&size);
    Some(Normalised {
        head,
        data: &d[data.start..data.start + data.len],
        claimed_data_bytes: data.claimed,
        pos: 0,
    })
}

/// A container presented to the decoder as the plain form of itself.
pub struct Normalised<'a> {
    head: Vec<u8>,
    data: &'a [u8],
    /// What the data chunk said it held, for a probe that reports claims.
    pub claimed_data_bytes: u64,
    pos: u64,
}

impl<'a> Normalised<'a> {
    /// The bytes of samples actually present.
    pub fn data_len(&self) -> usize {
        self.data.len()
    }

    fn len(&self) -> u64 {
        (self.head.len() + self.data.len()) as u64
    }
}

impl Read for Normalised<'_> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let head_len = self.head.len() as u64;
        let (src, offset) = if self.pos < head_len {
            (&self.head[..], self.pos as usize)
        } else {
            (self.data, (self.pos - head_len) as usize)
        };
        if offset >= src.len() {
            return Ok(0);
        }
        let n = buf.len().min(src.len() - offset);
        buf[..n].copy_from_slice(&src[offset..offset + n]);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for Normalised<'_> {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        let len = self.len() as i128;
        let target: i128 = match to {
            SeekFrom::Start(p) => p as i128,
            SeekFrom::End(off) => len + off as i128,
            SeekFrom::Current(off) => self.pos as i128 + off as i128,
        };
        if target < 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "seek before start",
            ));
        }
        self.pos = target.min(len) as u64;
        Ok(self.pos)
    }
}

impl MediaSource for Normalised<'_> {
    fn is_seekable(&self) -> bool {
        true
    }
    fn byte_len(&self) -> Option<u64> {
        Some(self.len())
    }
}

/// The plain form of a WAV, RF64, Wave64 or AIFF, or None when the bytes do
/// not even hold a data chunk: a header alone is not a file.
pub fn normalise(d: &[u8], format: Format) -> Option<Normalised<'_>> {
    match format {
        Format::Wav => rebuild(d, b"RIFF", b"WAVE", &walk_iff(d, false), b"data", false),
        Format::Aiff => {
            let kind: [u8; 4] = d.get(8..12)?.try_into().ok()?;
            rebuild(d, b"FORM", &kind, &walk_iff(d, true), b"SSND", true)
        }
        Format::Rf64 => {
            // The 32-bit sizes are placeholders; ds64 holds the real ones.
            // The walk clamps to the file either way, so ds64 is only read
            // for the data chunk's claim.
            let chunks: Vec<Chunk> = walk_iff(d, false)
                .into_iter()
                .filter(|c| &c.id != b"ds64")
                .collect();
            let mut n = rebuild(d, b"RIFF", b"WAVE", &chunks, b"data", false)?;
            if let Some(ds64) = walk_iff(d, false).iter().find(|c| &c.id == b"ds64") {
                if let Some(data_size) = le64(d, ds64.start + 8) {
                    n.claimed_data_bytes = data_size;
                }
            }
            Some(n)
        }
        Format::W64 => rebuild(d, b"RIFF", b"WAVE", &walk_w64(d), b"data", false),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wav(data_claim: u32, frames: u32) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(b"RIFF");
        v.extend_from_slice(&0xFFFF_FFF0u32.to_le_bytes());
        v.extend_from_slice(b"WAVE");
        v.extend_from_slice(b"fmt ");
        v.extend_from_slice(&16u32.to_le_bytes());
        for x in [1u16, 2] {
            v.extend_from_slice(&x.to_le_bytes());
        }
        for x in [44100u32, 44100 * 4] {
            v.extend_from_slice(&x.to_le_bytes());
        }
        for x in [4u16, 16] {
            v.extend_from_slice(&x.to_le_bytes());
        }
        v.extend_from_slice(b"data");
        v.extend_from_slice(&data_claim.to_le_bytes());
        for i in 0..frames * 2 {
            v.extend_from_slice(&(i as i16).to_le_bytes());
        }
        v
    }

    #[test]
    fn a_claim_past_the_end_is_clamped_to_the_bytes_there() {
        let d = wav(0xFFFF_FFF0, 100);
        let n = normalise(&d, Format::Wav).unwrap();
        assert_eq!(n.data_len(), 400);
        assert_eq!(n.claimed_data_bytes, 0xFFFF_FFF0);
        // The rebuilt header says 400 too.
        assert_eq!(&n.head[n.head.len() - 8..n.head.len() - 4], b"data");
        assert_eq!(
            u32::from_le_bytes(n.head[n.head.len() - 4..].try_into().unwrap()),
            400
        );
    }

    #[test]
    fn the_source_reads_header_then_data_and_seeks_across_both() {
        let d = wav(400, 100);
        let mut n = normalise(&d, Format::Wav).unwrap();
        let mut all = Vec::new();
        n.read_to_end(&mut all).unwrap();
        assert_eq!(all.len(), 44 + 400);
        assert_eq!(&all[..4], b"RIFF");
        assert_eq!(&all[44..48], &[0, 0, 1, 0]); // the first two samples, 0 and 1
        n.seek(SeekFrom::Start(46)).unwrap();
        let mut two = [0u8; 2];
        n.read_exact(&mut two).unwrap();
        assert_eq!(two, [1, 0]);
        assert_eq!(n.seek(SeekFrom::End(0)).unwrap(), 444);
        assert_eq!(n.read(&mut two).unwrap(), 0);
    }

    #[test]
    fn a_header_with_no_data_chunk_is_not_a_file() {
        let d = &wav(400, 100)[..36];
        assert!(normalise(d, Format::Wav).is_none());
    }

    #[test]
    fn sniffing_is_by_bytes() {
        assert_eq!(sniff(b"fLaC...."), Format::Flac);
        assert_eq!(sniff(b"OggS...."), Format::Ogg);
        assert_eq!(sniff(b"RIFF....WAVE"), Format::Wav);
        assert_eq!(sniff(b"RF64....WAVE"), Format::Rf64);
        assert_eq!(sniff(b"FORM....AIFC"), Format::Aiff);
        assert_eq!(sniff(b"ID3....."), Format::Mp3);
        assert_eq!(sniff(&[0xFF, 0xFB, 0x90, 0x00]), Format::Mp3);
        assert_eq!(sniff(&[0xFF, 0xEB, 0x90, 0x00]), Format::Unknown); // the reserved version
        assert_eq!(sniff(b"this is not audio"), Format::Unknown);
        assert_eq!(sniff(b"RIF"), Format::Unknown);
        let mut w64 = W64_RIFF.to_vec();
        w64.extend_from_slice(&[0; 24]);
        assert_eq!(sniff(&w64), Format::W64);
    }
}
