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
    body(d).1
}

/// The bytes the decoder reads, and what they are. An ID3v2 tag in front
/// says nothing about what follows it (an MP3 nearly always, a FLAC from
/// some taggers), so the tag is stepped over and what is behind it is
/// sniffed; an MP3's ID3v1 tag at the end is cut off the same way. The
/// decoder never sees a tag, so it has no reason to look past the format
/// it was given.
pub fn body(d: &[u8]) -> (&[u8], Format) {
    let mut rest = d;
    while rest.starts_with(b"ID3") {
        match after_id3v2(rest) {
            Some(r) => rest = r,
            // A tag cut short: an MP3 as far as anyone can tell, and the MP3
            // reader will say whether it is one.
            None => return (rest, Format::Mp3),
        }
    }
    let format = match sniff_bytes(rest) {
        // A tag with nothing readable behind it is taken for an MP3 too.
        Format::Unknown if rest.len() != d.len() => Format::Mp3,
        f => f,
    };
    if format == Format::Mp3 && rest.len() >= 128 && rest[rest.len() - 128..].starts_with(b"TAG") {
        rest = &rest[..rest.len() - 128];
    }
    (rest, format)
}

/// The bytes after an ID3v2 tag at the front of `d`, if the whole tag is
/// there: a ten-byte header whose size is four seven-bit bytes, and a
/// ten-byte footer when flag bit 4 says so.
fn after_id3v2(d: &[u8]) -> Option<&[u8]> {
    let h = d.get(..10)?;
    let size = h[6..10].iter().try_fold(0usize, |n, &b| {
        (b < 0x80).then(|| (n << 7) | usize::from(b))
    })?;
    let footer = if h[5] & 0x10 != 0 { 10 } else { 0 };
    d.get(10 + size + footer..)
}

fn sniff_bytes(d: &[u8]) -> Format {
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

/// The most channels a fmt or COMM chunk may declare. The decoder's channel
/// mask stops far below this, so no file it could read is lost. What is
/// refused is a count the decoder multiplies in 16 bits before it checks it:
/// 45,314 channels at 16 bits overflowed there and panicked (the finding is
/// in corpus/regressions).
const MAX_CHANNELS: u16 = 64;

fn le16(d: &[u8], at: usize) -> Option<u16> {
    d.get(at..at + 2).map(|b| u16::from_le_bytes([b[0], b[1]]))
}
fn be16(d: &[u8], at: usize) -> Option<u16> {
    d.get(at..at + 2).map(|b| u16::from_be_bytes([b[0], b[1]]))
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

/// The channel count the format chunk declares: a WAVE fmt's second field,
/// an AIFF COMM's first. A file declares its format exactly once: the decoder
/// reads every chunk, the ones after the data too, so a second fmt or COMM
/// would reach it unchecked (the nightly fuzz run's third finding). A file
/// without one, or with two, is not a file the decoder could read.
fn declared_channels(d: &[u8], chunks: &[Chunk], big_endian: bool) -> Option<u16> {
    let id: &[u8; 4] = if big_endian { b"COMM" } else { b"fmt " };
    let mut found = chunks.iter().filter(|c| &c.id == id);
    let format = found.next()?;
    if found.next().is_some() {
        return None;
    }
    let upto = &d[..format.start + format.len];
    if big_endian {
        be16(upto, format.start)
    } else {
        le16(upto, format.start + 2)
    }
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
    let channels = declared_channels(d, chunks, big_endian)?;
    if channels == 0 || channels > MAX_CHANNELS {
        return None;
    }
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

    fn with_channels(mut d: Vec<u8>, n: u16) -> Vec<u8> {
        d[22..24].copy_from_slice(&n.to_le_bytes());
        d
    }

    #[test]
    fn a_channel_count_the_decoder_cannot_hold_is_refused() {
        // 45,314 channels at 16 bits overflowed the decoder's 16-bit block
        // align arithmetic: the nightly fuzz run's first finding.
        assert!(normalise(&with_channels(wav(400, 100), 45_314), Format::Wav).is_none());
        assert!(normalise(&with_channels(wav(400, 100), 0), Format::Wav).is_none());
        assert!(normalise(&with_channels(wav(400, 100), MAX_CHANNELS + 1), Format::Wav).is_none());
        assert!(normalise(&with_channels(wav(400, 100), MAX_CHANNELS), Format::Wav).is_some());
        assert!(normalise(&with_channels(wav(400, 100), 2), Format::Wav).is_some());
    }

    #[test]
    fn a_second_format_chunk_is_refused() {
        // The nightly fuzz run's third finding: a well-formed fmt, then a
        // data chunk, then a second fmt declaring 45,314 channels. The decoder
        // reads every chunk, so a file may declare its format exactly once.
        let mut d = wav(400, 100);
        d.extend_from_slice(b"fmt ");
        d.extend_from_slice(&16u32.to_le_bytes());
        for x in [1u16, 45_314] {
            d.extend_from_slice(&x.to_le_bytes());
        }
        for x in [44100u32, 44100 * 4] {
            d.extend_from_slice(&x.to_le_bytes());
        }
        for x in [4u16, 16] {
            d.extend_from_slice(&x.to_le_bytes());
        }
        assert!(normalise(&d, Format::Wav).is_none());
    }

    fn aiff(channels: u16) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(b"FORM");
        v.extend_from_slice(&0u32.to_be_bytes());
        v.extend_from_slice(b"AIFF");
        v.extend_from_slice(b"COMM");
        v.extend_from_slice(&18u32.to_be_bytes());
        v.extend_from_slice(&channels.to_be_bytes());
        v.extend_from_slice(&100u32.to_be_bytes());
        v.extend_from_slice(&16u16.to_be_bytes());
        v.extend_from_slice(&[0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0]); // 44100 as an 80-bit float
        v.extend_from_slice(b"SSND");
        v.extend_from_slice(&408u32.to_be_bytes());
        v.extend_from_slice(&[0u8; 8]);
        v.extend_from_slice(&[0u8; 400]);
        v
    }

    #[test]
    fn an_aiff_comm_is_held_to_the_same_count() {
        assert!(normalise(&aiff(2), Format::Aiff).is_some());
        assert!(normalise(&aiff(MAX_CHANNELS), Format::Aiff).is_some());
        assert!(normalise(&aiff(0), Format::Aiff).is_none());
        assert!(normalise(&aiff(MAX_CHANNELS + 1), Format::Aiff).is_none());
        assert!(normalise(&aiff(45_314), Format::Aiff).is_none());
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
