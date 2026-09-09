// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Event tags: a stable 32-bit hash of a short string, and the two names
//! clockwork reserves (0 is not a name — it is the flush wildcard).
//!
//! # Why this is spelled twice
//!
//! `sched_tag_hash` also exists, `constexpr`, in `src/scheduler/Scheduler.h`,
//! and it has to: `test/test_scheduler.cpp` asserts its results in
//! `static_assert`, which no call across a C ABI can satisfy. That is not an
//! oversight but the nature of the value — the test calls these "wire
//! constants, not an implementation detail", because a guest computes them on
//! its own side of the ABI too. So there are several implementations by
//! design, and what keeps them honest is that each pins the SAME frozen
//! results. The ones below are the values `test/test_scheduler.cpp` freezes;
//! change one side and that side's test fails.

/// FNV-1a, 32-bit, over exactly `bytes` — not to a NUL.
///
/// Never returns 0: 0 is the flush wildcard ("cancel everything"), so a tag
/// string whose raw hash lands on 0 would silently turn every flush of it into
/// a flush of the queue. It is clamped to 1 instead. Exactly one four-byte
/// input in the test suite exercises that clamp.
pub const fn tag_hash(bytes: &[u8]) -> u32 {
    let mut h: u32 = 2166136261;
    let mut i = 0;
    while i < bytes.len() {
        h ^= bytes[i] as u32;
        h = h.wrapping_mul(16777619);
        i += 1;
    }
    if h != 0 {
        h
    } else {
        1
    }
}

/// User-scheduled events; cancelled on a run stop.
pub const TAG_DEFAULT: u32 = tag_hash(b"default");
/// Graph (synth) events, distinct for the same reason.
pub const TAG_SYNTH: u32 = tag_hash(b"synth");

#[cfg(test)]
mod tests {
    use super::*;

    // The frozen values from test/test_scheduler.cpp, byte for byte. If this
    // file and Scheduler.h ever disagree, one of these two suites fails.
    #[test]
    fn frozen_wire_constants() {
        assert_eq!(tag_hash(b""), 0x811C9DC5, "FNV-1a 32-bit offset basis");
        assert_eq!(tag_hash(b"keep"), 0xEE7B9448);
        assert_eq!(tag_hash(b"flushme"), 0x3071C5EF);
        assert_eq!(TAG_DEFAULT, 0x933B5BDE);
        assert_eq!(TAG_SYNTH, 0xC0214F59);
    }

    #[test]
    fn never_returns_the_wildcard() {
        // The one four-byte input whose raw FNV-1a is exactly 0.
        assert_eq!(tag_hash(&[0xCC, 0x24, 0x31, 0xC4]), 1);
    }

    #[test]
    fn covers_exactly_the_bytes_given() {
        assert_eq!(tag_hash(b"keep"), tag_hash(&b"keepXX"[..4]));
        assert_ne!(tag_hash(&b"keep"[..3]), tag_hash(b"keep"));
    }

    #[test]
    fn the_reserved_tags_are_distinct_and_live() {
        // A tag the store has never heard of. The store groups by an opaque
        // key; which strings exist is the caller's business, so this stands
        // in for one of theirs.
        const CALLER: u32 = tag_hash(b"anything at all");
        for t in [TAG_DEFAULT, TAG_SYNTH, CALLER] {
            assert_ne!(t, 0);
        }
        assert_ne!(TAG_DEFAULT, TAG_SYNTH);
        assert_ne!(TAG_DEFAULT, CALLER);
        assert_ne!(TAG_SYNTH, CALLER);
    }
}
