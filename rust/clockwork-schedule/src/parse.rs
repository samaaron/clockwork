// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The two wire forms that carry a time, parsed in one place.
//!
//!   * a timestamped OSC bundle — `"#bundle"` plus an 8-byte NTP timetag; and
//!   * the flat twin, `"/clockwork/schedule <timetag> <blob>"`: a single
//!     addressed inner message clockwork re-ingests when it comes due.
//!
//! Nothing here knows what the inner blob is; it is handed back as an offset
//! and a length into the message and never inspected.
//!
//! The flat form is read with the tree's one OSC codec (`clockwork_osc::wire`),
//! so no byte offset is written down here, and the address is the reserved
//! prefix's own (`clockwork_sys!("schedule")`): changing the prefix needs no
//! edit here.

use clockwork_osc::wire::{Arg, Message};
use clockwork_osc::{clockwork_sys, Timetag};

/// The flat form's address, built from the reserved prefix.
pub const SCHEDULE_ADDR: &str = clockwork_sys!("schedule");

/// The result of parsing the flat form. The blob is named by its offset into
/// the message it was parsed from, so the result carries no pointer and no
/// lifetime; [`Packet::blob`] hands it back as a slice of that message.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Packet {
    pub ok: bool,
    pub when: Timetag,
    pub blob_off: usize,
    pub blob_len: u32,
}

impl Packet {
    /// The blob, as a slice of the message this was parsed from. Empty when
    /// the parse failed.
    pub fn blob<'a>(&self, msg: &'a [u8]) -> &'a [u8] {
        if !self.ok { return &[]; }
        &msg[self.blob_off..self.blob_off + self.blob_len as usize]
    }
}

/// A timestamped OSC bundle: the marker AND a whole timetag. The size check is
/// first — a truncated bundle is not a bundle even with the marker intact.
///
#[inline]
pub fn is_bundle(data: &[u8]) -> bool {
    data.len() >= 16 && &data[..7] == b"#bundle"
}

/// The 8-byte big-endian NTP timetag at offset 8 of a bundle; immediately for
/// anything too short to hold one — call [`is_bundle`] first.
#[inline]
pub fn bundle_timetag(bundle: &[u8]) -> Timetag {
    if bundle.len() < 16 { return Timetag::default(); }
    Timetag::from_ntp(be_u64(&bundle[8..16]))
}

/// Parse `"/clockwork/schedule <timetag> <blob>"`.
///
/// The timetag is an OSC timetag, as `'t'` or the int64 `'h'` (full
/// sub-sample resolution), or, as a convenience, a `'d'`/`'f'` NTP-seconds
/// value. Anything that is not a whole OSC message
/// ([`clockwork_osc::wire::Message::parse`]), or does not open with a time and
/// a blob, is refused; the parser never reads past `s`.
pub fn parse(s: &[u8]) -> Packet {
    let miss = Packet::default();
    let Ok(message) = Message::parse(s) else { return miss };
    // The whole address, so "/clockwork/scheduled" is not a match.
    if message.address() != SCHEDULE_ADDR {
        return miss;
    }
    let mut args = message.args();
    let when = match args.next() {
        // An OSC timetag, as `t` (OSC's own type) or `h` (an int64).
        Some(Arg::TimeTag(t)) => t,
        Some(Arg::Long(v)) => Timetag::from_ntp(v as u64),
        Some(Arg::Double(v)) => Timetag::from_ntp_seconds(v),
        Some(Arg::Float(v)) => Timetag::from_ntp_seconds(f64::from(v)),
        _ => return miss,
    };
    let Some(Arg::Blob(blob)) = args.next() else { return miss };
    // The blob is a slice of `s`: where it begins is how far its first byte
    // is from the message's.
    let blob_off = blob.as_ptr() as usize - s.as_ptr() as usize;
    Packet { ok: true, when, blob_off, blob_len: blob.len() as u32 }
}

#[inline]
fn be_u64(b: &[u8]) -> u64 {
    let mut v: u64 = 0;
    for &x in &b[..8] {
        v = (v << 8) | x as u64;
    }
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Address length, NUL excluded.
    const ADDR_LEN: usize = SCHEDULE_ADDR.len();
    /// Bytes the address occupies on the wire: NUL included, padded to 4.
    const ADDR_PAD: usize = (ADDR_LEN + 4) & !3;

    fn padded(out: &mut Vec<u8>, s: &str) {
        out.extend_from_slice(s.as_bytes());
        out.push(0);
        while out.len() % 4 != 0 {
            out.push(0);
        }
    }

    fn sched_msg(typetag: &str, time: &[u8], blob: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        padded(&mut v, SCHEDULE_ADDR);
        padded(&mut v, typetag);
        v.extend_from_slice(time);
        v.extend_from_slice(&(blob.len() as u32).to_be_bytes());
        v.extend_from_slice(blob);
        while v.len() % 4 != 0 {
            v.push(0);
        }
        v
    }

    const FILLER: [u8; 8] = [0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4];

    fn run(m: &[u8]) -> Packet {
        parse(m)
    }

    #[test]
    fn offsets_track_the_prefix() {
        // Not "12 and 20": derived, and asserted against the prefix itself.
        assert_eq!(ADDR_LEN, SCHEDULE_ADDR.len());
        assert_eq!(ADDR_PAD, (SCHEDULE_ADDR.len() + 4) & !3);
        assert!(SCHEDULE_ADDR.starts_with(clockwork_sys!()));
    }

    #[test]
    fn bundles_need_marker_and_whole_timetag() {
        let mut b = Vec::new();
        padded(&mut b, "#bundle");
        b.extend_from_slice(&(((7u64) << 32) | 0x8000_0000).to_be_bytes());
        assert_eq!(b.len(), 16);
        assert!(is_bundle(&b));
        assert!(!is_bundle(&b[..15]));
        assert_eq!(bundle_timetag(&b).ntp(), (7u64 << 32) | 0x8000_0000);
        assert!(bundle_timetag(&b[..15]).is_immediate(), "too short to hold one");
        let mut wrong = b.clone();
        wrong[6] = b'X';
        assert!(!is_bundle(&wrong));
        assert!(!is_bundle(&[]));
    }

    #[test]
    fn the_four_timetag_spellings() {
        let when = (0x123u64 << 32) | 0x4567_89AB;
        let r = run(&sched_msg(",hb", &when.to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when.ntp(), when);
        assert_eq!(r.blob_len, 8);

        // OSC's own timetag type: the same eight bytes as `h`, so a client
        // whose library writes a time as a time is understood.
        let r = run(&sched_msg(",tb", &when.to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when.ntp(), when);
        assert_eq!(r.blob_len, 8);

        let r = run(&sched_msg(",db", &3600.5f64.to_bits().to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when.ntp(), (3600u64 << 32) | 0x8000_0000);

        let r = run(&sched_msg(",fb", &2.25f32.to_bits().to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when.ntp(), (2u64 << 32) | 0x4000_0000);
    }

    #[test]
    fn an_empty_blob_is_not_a_failure() {
        let m = sched_msg(",hb", &42u64.to_be_bytes(), &[]);
        let r = run(&m);
        assert!(r.ok);
        assert_eq!(r.when.ntp(), 42);
        assert_eq!(r.blob_len, 0);
        assert!(r.blob(&m).is_empty(), "an empty blob is an empty slice, and a parse that worked");
    }

    #[test]
    fn every_truncation_is_refused_and_nothing_is_read_past_the_end() {
        for spelling in [",hb", ",tb", ",fb"] {
            let time: Vec<u8> = if spelling != ",fb" {
                1u64.to_be_bytes().to_vec()
            } else {
                1.0f32.to_bits().to_be_bytes().to_vec()
            };
            let m = sched_msg(spelling, &time, &FILLER);
            for n in 0..m.len() {
                // Copy the prefix so a read past `n` would be a real overrun
                // under Miri or ASan, not merely a value from further along.
                let cut = m[..n].to_vec();
                assert!(!parse(&cut).ok, "{spelling} n={n}");
            }
            assert!(run(&m).ok);
        }
    }

    #[test]
    fn a_lying_blob_length_is_refused() {
        let mut m = sched_msg(",hb", &1u64.to_be_bytes(), &FILLER);
        let args = ADDR_PAD + 4;
        m[args + 8 + 2] = 0x01; // 8 -> 264, past the end
        assert!(!run(&m).ok);
        // ... and one that would wrap a pointer add rather than exceed it.
        for b in &mut m[args + 8..args + 12] {
            *b = 0xFF;
        }
        assert!(!run(&m).ok);
    }

    #[test]
    fn malformed_headers_are_refused() {
        let good = sched_msg(",hb", &1u64.to_be_bytes(), &FILLER);
        assert!(run(&good).ok);
        for n in 0..ADDR_PAD + 8 {
            assert!(!parse(&good[..n]).ok);
        }
        let mut m = good.clone();
        m[ADDR_LEN - 1] = b'X';
        assert!(!run(&m).ok);

        let mut m = good.clone(); // "/clockwork/scheduled" is a different address
        m[ADDR_LEN] = b'd';
        assert!(!run(&m).ok);

        let mut m = good.clone();
        m[ADDR_PAD] = b';';
        assert!(!run(&m).ok);

        for t in [b'i', b's', b'b', 0u8] {
            let mut m = good.clone();
            m[ADDR_PAD + 1] = t;
            assert!(!run(&m).ok);
        }
        for t in [b'i', b's', b'f', 0u8] {
            let mut m = good.clone();
            m[ADDR_PAD + 2] = t;
            assert!(!run(&m).ok);
        }
    }

    // It is a whole OSC message or it is refused: a blob carries its padding
    // to four bytes, and what follows the blob is read as arguments too.
    #[test]
    fn the_message_is_held_to_osc_as_a_whole() {
        let odd = sched_msg(",hb", &1u64.to_be_bytes(), &FILLER[..5]);
        let r = run(&odd);
        assert!(r.ok);
        assert_eq!(r.blob(&odd), &FILLER[..5]);
        assert!(!parse(&odd[..odd.len() - 3]).ok, "a blob without its padding");

        let mut more = sched_msg(",hbi", &1u64.to_be_bytes(), &FILLER);
        more.extend_from_slice(&7i32.to_be_bytes());
        assert!(run(&more).ok, "an argument after the blob is not the scheduler's to refuse");
        assert_eq!(run(&more).blob(&more), &FILLER);
        let unknown = sched_msg(",hbq", &1u64.to_be_bytes(), &FILLER);
        assert!(!run(&unknown).ok, "a type tag OSC does not have");
    }

    #[test]
    fn a_failed_parse_is_wholly_inert() {
        let junk = [0u8; 24];
        let r = run(&junk);
        assert!(!r.ok);
        assert_eq!(r.when.ntp(), 0);
        assert!(r.blob(&junk).is_empty());
        assert_eq!(r.blob_len, 0);
        assert!(!parse(&[]).ok);
    }
}
