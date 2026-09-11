// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The two wire forms that carry a time, parsed in one place.
//!
//!   * a timestamped OSC bundle — `"#bundle"` plus an 8-byte NTP timetag; and
//!   * the flat twin, `"/clockwork/schedule <timetag> <blob>"`: a single
//!     addressed inner message clockwork re-ingests when it comes due.
//!
//! Pure byte work. Nothing here knows what the inner blob is; it is handed
//! back as a borrowed pointer and length and never inspected.
//!
//! # Every offset is derived, none is written down
//!
//! The C++ parser this replaces once carried hand-counted byte offsets, and
//! they were silently wrong the moment the reserved prefix changed length —
//! the parser kept reading, at the wrong bytes. So the constants below are
//! computed from `clockwork_sys!("schedule")`, the one place the Rust tree spells
//! the prefix (`clockwork-osc`). Changing the prefix moves them; nothing here
//! needs editing, and nothing here may be replaced by a number.

use clockwork_osc::clockwork_sys;

/// The flat form's address, built from the reserved prefix.
pub const SCHEDULE_ADDR: &str = clockwork_sys!("schedule");

/// Address length, NUL excluded.
const ADDR_LEN: usize = SCHEDULE_ADDR.len();
/// Bytes the address occupies on the wire: NUL included, padded to 4.
const ADDR_PAD: usize = (ADDR_LEN + 4) & !3;

/// One second in NTP units — a 64-bit fixed-point timestamp is 32 bits of
/// seconds over 32 bits of fraction (RFC 5905 §6), so a second is 2^32. The
/// same constant as `clockwork::kNtpUnitsPerSecond` in `src/clock/clock_math.h`,
/// and `test_schedule_parse.cpp` checks the two formulas agree by comparing
/// their results directly.
const NTP_UNITS_PER_SECOND: f64 = 4294967296.0;

/// The result of parsing the flat form. The blob is named by its offset into
/// the message it was parsed from, so the result carries no pointer and no
/// lifetime; [`Packet::blob`] hands it back as a slice of that message.
#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Packet {
    pub ok: bool,
    pub when: i64,
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

/// NTP seconds (since 1900) into an OSC 32.32 fixed-point timetag.
///
/// Rust's float-to-integer casts saturate where C++'s are undefined, so an NTP
/// value outside the 32-bit range clamps here rather than producing whatever
/// the target's conversion instruction happened to leave behind. For every
/// value in range the two are bit-identical.
#[inline]
pub fn ntp_to_timetag(ntp: f64) -> i64 {
    let s = ntp as u32;
    let f = ((ntp - s as f64) * NTP_UNITS_PER_SECOND) as u32;
    (((s as u64) << 32) | f as u64) as i64
}

/// A timestamped OSC bundle: the marker AND a whole timetag. The size check is
/// first — a truncated bundle is not a bundle even with the marker intact.
///
#[inline]
pub fn is_bundle(data: &[u8]) -> bool {
    data.len() >= 16 && &data[..7] == b"#bundle"
}

/// The 8-byte big-endian NTP timetag at offset 8 of a bundle; 0 for anything
/// too short to hold one — call [`is_bundle`] first.
#[inline]
pub fn bundle_timetag(bundle: &[u8]) -> u64 {
    if bundle.len() < 16 { return 0; }
    be_u64(&bundle[8..16])
}

/// Parse `"/clockwork/schedule <timetag> <blob>"`.
///
/// The timetag is the OSC int64 `'h'` (full sub-sample resolution) or, as a
/// convenience, a `'d'`/`'f'` NTP-seconds value. Anything malformed or
/// truncated is refused; the parser never reads past `size`.
pub fn parse(s: &[u8]) -> Packet {
    let miss = Packet::default();
    let size = s.len();
    if size < ADDR_PAD + 8 {
        return miss;
    }
    // The NUL is part of the check, so "/clockwork/scheduled" is not a match.
    if &s[..ADDR_LEN] != SCHEDULE_ADDR.as_bytes() || s[ADDR_LEN] != 0 {
        return miss;
    }

    let tt = &s[ADDR_PAD..]; // ",<t>b\0" — three bytes guaranteed by the size check
    let mut p = ADDR_PAD + 4; // arguments start past the type tag, padded to 4
    let end = size;
    if tt[0] != b',' {
        return miss;
    }
    let when = match tt[1] {
        b'h' => {
            if p + 8 > end {
                return miss;
            }
            let v = be_u64(&s[p..p + 8]);
            p += 8;
            v as i64
        }
        b'd' => {
            if p + 8 > end {
                return miss;
            }
            let v = f64::from_bits(be_u64(&s[p..p + 8]));
            p += 8;
            ntp_to_timetag(v)
        }
        b'f' => {
            if p + 4 > end {
                return miss;
            }
            let v = f32::from_bits(be_u32(&s[p..p + 4]));
            p += 4;
            ntp_to_timetag(v as f64)
        }
        _ => return miss,
    };
    if tt[2] != b'b' {
        return miss;
    }
    if p + 4 > end {
        return miss;
    }
    let n = be_u32(&s[p..p + 4]);
    p += 4;
    // Checked, so a lying blob length cannot wrap into a range that looks
    // contained. C++ compared raw pointers here and would have wrapped.
    match (p as u64).checked_add(n as u64) {
        Some(e) if e <= end as u64 => {}
        _ => return miss,
    }
    Packet { ok: true, when, blob_off: p, blob_len: n }
}

#[inline]
fn be_u64(b: &[u8]) -> u64 {
    let mut v: u64 = 0;
    for &x in &b[..8] {
        v = (v << 8) | x as u64;
    }
    v
}

#[inline]
fn be_u32(b: &[u8]) -> u32 {
    ((b[0] as u32) << 24) | ((b[1] as u32) << 16) | ((b[2] as u32) << 8) | b[3] as u32
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn ntp_packs_as_32_32() {
        assert_eq!(ntp_to_timetag(0.0), 0);
        assert_eq!(ntp_to_timetag(1.0), 1i64 << 32);
        assert_eq!(ntp_to_timetag(1.5), (1i64 << 32) | 0x8000_0000);
        assert_eq!(ntp_to_timetag(2.25), (2i64 << 32) | 0x4000_0000);
        assert_eq!(ntp_to_timetag(3600.0) >> 32, 3600);
    }

    #[test]
    fn bundles_need_marker_and_whole_timetag() {
        let mut b = Vec::new();
        padded(&mut b, "#bundle");
        b.extend_from_slice(&(((7u64) << 32) | 0x8000_0000).to_be_bytes());
        assert_eq!(b.len(), 16);
        assert!(is_bundle(&b));
        assert!(!is_bundle(&b[..15]));
        assert_eq!(bundle_timetag(&b), (7u64 << 32) | 0x8000_0000);
        assert_eq!(bundle_timetag(&b[..15]), 0, "too short to hold one");
        let mut wrong = b.clone();
        wrong[6] = b'X';
        assert!(!is_bundle(&wrong));
        assert!(!is_bundle(&[]));
    }

    #[test]
    fn the_three_timetag_spellings() {
        let when = (0x123i64 << 32) | 0x4567_89AB;
        let r = run(&sched_msg(",hb", &(when as u64).to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when, when);
        assert_eq!(r.blob_len, 8);

        let r = run(&sched_msg(",db", &3600.5f64.to_bits().to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when, (3600i64 << 32) | 0x8000_0000);

        let r = run(&sched_msg(",fb", &2.25f32.to_bits().to_be_bytes(), &FILLER));
        assert!(r.ok);
        assert_eq!(r.when, (2i64 << 32) | 0x4000_0000);
    }

    #[test]
    fn an_empty_blob_is_not_a_failure() {
        let m = sched_msg(",hb", &42u64.to_be_bytes(), &[]);
        let r = run(&m);
        assert!(r.ok);
        assert_eq!(r.when, 42);
        assert_eq!(r.blob_len, 0);
        assert!(r.blob(&m).is_empty(), "an empty blob is an empty slice, and a parse that worked");
    }

    #[test]
    fn every_truncation_is_refused_and_nothing_is_read_past_the_end() {
        for spelling in [",hb", ",fb"] {
            let time: Vec<u8> = if spelling == ",hb" {
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

        for t in [b'i', b's', b't', b'b', 0u8] {
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

    #[test]
    fn a_failed_parse_is_wholly_inert() {
        let junk = [0u8; 24];
        let r = run(&junk);
        assert!(!r.ok);
        assert_eq!(r.when, 0);
        assert!(r.blob(&junk).is_empty());
        assert_eq!(r.blob_len, 0);
        assert!(!parse(&[]).ok);
    }
}
