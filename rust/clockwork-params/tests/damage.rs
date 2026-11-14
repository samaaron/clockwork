// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A reader over bytes that are not what they should be, seeded so a failure
//! repeats: a mapped region is another process's memory, and a host's words are a
//! host's. It reads what it can, and never reads past the bytes or panics.
use clockwork_params::*;

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: usize) -> usize { (self.next() % n as u64) as usize }
}

fn fixture() -> Vec<u32> {
    let bytes = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../../test/fixtures/params_region.bin")).unwrap();
    let mut words = vec![0u32; bytes.len().div_ceil(4)];
    // SAFETY: the words are as long as the bytes.
    unsafe { core::ptr::copy_nonoverlapping(bytes.as_ptr(), words.as_mut_ptr().cast::<u8>(), bytes.len()) };
    words
}

fn exercise(p: &Param, words: &str) {
    let e = p.entry();
    for v in [e.min, e.max, e.def, 0.0, f32::NAN, f32::INFINITY, -f32::INFINITY, f32::MAX] {
        let _ = p.position_of(v);
        let _ = p.text_of(v);
        let _ = p.held(v);
    }
    for position in [0.0, 0.5, 1.0, -1.0, 2.0, f32::NAN] {
        let _ = p.value_at(position);
    }
    let _ = p.value_of_text(words);
    let _ = (p.name(), p.full_name(), p.doc(), p.unit_text(), p.value_name(0));
    let _ = p.value_names().count();
}

#[test]
fn damaged_regions_are_read_or_refused_and_nothing_panics() {
    let clean = fixture();
    let mut rng = Rng(0x9a7a_5eed);
    let mut read = 0;
    for _ in 0..3000 {
        let mut words = clean.clone();
        // Mostly the header and the entries, where the offsets and lengths lie.
        for _ in 0..1 + rng.below(8) {
            let at = if rng.below(4) == 0 { rng.below(words.len()) } else { rng.below(16 + 256 + 256 * 24 + 8).min(words.len() - 1) };
            words[at] = match rng.below(4) {
                0 => rng.next() as u32,
                1 => u32::MAX,
                2 => 0,
                _ => words[at] ^ (1 << rng.below(32)),
            };
        }
        let bytes = words.len() * 4;
        // SAFETY: the words outlive the reader, and this test is the only thread.
        let Some(mut r) = (unsafe { Reader::from_raw(words.as_ptr().cast::<u8>(), bytes) }) else { continue };
        read += 1;
        let _ = r.refresh();
        let _ = r.value(rng.next() as u32);
        for p in r.params() {
            let _ = r.value(p.slot());
            exercise(p, "1.2 kHz");
        }
    }
    assert!(read > 500, "the damage left something to read: {read}");
}

#[test]
fn any_words_and_any_run_are_taken_or_refused() {
    let mut rng = Rng(0x7e47_c0de);
    let mut words: Vec<&str> = Vec::new();
    let kinds = [CLOCKWORK_PARAM_CONTINUOUS, CLOCKWORK_PARAM_STEPPED, CLOCKWORK_PARAM_SWITCH, CLOCKWORK_PARAM_NOTE, CLOCKWORK_PARAM_CHOICE, CLOCKWORK_PARAM_GROUP, 0, 99];
    for round in 0..5000 {
        let mut e = ClockworkParamEntry::new(1, "p", kinds[round % kinds.len()], -60.0, 12000.0, 0.0);
        e.unit = rng.below(10) as u32;
        e.taper = [0.0, -1.0, 0.5, 3.0, f32::NAN, -7.0][rng.below(6)];
        if rng.below(8) == 0 { e.min = f32::from_bits(rng.next() as u32); }
        if rng.below(8) == 0 { e.max = f32::from_bits(rng.next() as u32); }
        let run: Vec<u8> = (0..rng.below(64)).map(|_| rng.next() as u8).collect();
        // SAFETY: an entry is plain words.
        let bytes = unsafe { core::slice::from_raw_parts((&raw const e).cast::<u8>(), size_of::<ClockworkParamEntry>()) };
        let p = Param::from_bytes(bytes, &run).unwrap();
        words.clear();
        for _ in 0..rng.below(12) {
            words.push(["C", "#", "b", "-", "+", ".", "9", "0", " ", "kHz", "é", "off", "99999999999999999999", "2147483647"][rng.below(14)]);
        }
        exercise(&p, &words.concat());
    }
}
