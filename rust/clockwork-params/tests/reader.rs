// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The reader against the bytes the engine wrote: test/fixtures/params_region.bin,
//! which test_params.cpp holds to what the engine does and the web reader reads too.
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use clockwork_params::*;

/// The fixture in words, so its bytes lie on a word as a mapped region's do.
fn fixture() -> Vec<u32> {
    let bytes = std::fs::read(concat!(env!("CARGO_MANIFEST_DIR"), "/../../test/fixtures/params_region.bin")).unwrap();
    let mut words = vec![0u32; bytes.len().div_ceil(4)];
    // SAFETY: the words are as long as the bytes.
    unsafe { core::ptr::copy_nonoverlapping(bytes.as_ptr(), words.as_mut_ptr().cast::<u8>(), bytes.len()) };
    words
}
fn bytes_of(words: &[u32]) -> &[u8] {
    // SAFETY: any u32 is four bytes.
    unsafe { core::slice::from_raw_parts(words.as_ptr().cast::<u8>(), words.len() * 4) }
}
const GENERATION_WORD: usize = 4;

#[test]
fn it_reads_what_the_engine_wrote() {
    let words = fixture();
    let mut r = Reader::new(bytes_of(&words)).expect("a table");
    assert!(r.refresh(), "the first refresh has everything to read");
    assert_eq!(r.generation(), 1);
    assert_eq!(r.params().len(), 5);

    let tone = r.find(9).unwrap();
    assert!(tone.is_group());
    assert_eq!(tone.name(), "tone");
    assert_eq!(tone.doc(), Some("What shapes the tone"));

    let cutoff = r.find(1).unwrap();
    assert_eq!(cutoff.name(), "cutoff");
    assert_eq!(cutoff.full_name(), "Filter cutoff");
    assert_eq!(cutoff.doc(), Some("Where the filter closes"));
    assert_eq!(cutoff.entry().unit, CLOCKWORK_PARAM_UNIT_HERTZ);
    assert_eq!(cutoff.entry().taper, CLOCKWORK_PARAM_TAPER_LOG);
    assert_eq!(cutoff.entry().group, 9);
    assert_eq!(r.value(cutoff.slot()), 440.0);

    let steps = r.find(2).unwrap();
    assert!(steps.is_hidden());
    assert_eq!(steps.full_name(), "steps", "with no full name, the name is the name");
    assert_eq!(steps.unit_text(), Some("steps"));
    assert_eq!(r.value(steps.slot()), 3.0);

    let on = r.with_role(CLOCKWORK_PARAM_ROLE_BYPASS).unwrap();
    assert_eq!(on.id(), 3);
    assert_eq!(r.value(on.slot()), 1.0);
    assert!(r.with_role(CLOCKWORK_PARAM_ROLE_TEMPO).is_none());

    let wave = r.find(4).unwrap();
    assert_eq!(wave.value_names().collect::<Vec<_>>(), ["saw", "square", "é"]);
    assert_eq!(wave.value_name(2), Some("é"));
    assert_eq!(wave.value_name(3), None);
}

#[test]
fn nothing_moved_is_nothing_to_read() {
    let words = fixture();
    let mut r = Reader::new(bytes_of(&words)).unwrap();
    assert!(r.refresh());
    let before = r.params().as_ptr();
    assert!(!r.refresh());
    assert_eq!(r.params().as_ptr(), before, "no new facts, no new allocation");
}

#[test]
fn it_reads_again_when_the_generation_moves_and_waits_out_a_define() {
    let mut words = fixture();
    let base = words.as_mut_ptr();
    // SAFETY: the words outlive the reader, and this test is the only thread.
    let mut r = unsafe { Reader::from_raw(base.cast::<u8>().cast_const(), words.len() * 4) }.unwrap();
    assert!(r.refresh());
    let entries = r.params()[1].slot();
    // SAFETY: the fixture starts with a header, and the words lie on a word.
    let header = unsafe { core::ptr::read(base.cast::<ClockworkParamsHeader>()) };
    let name_at = |slot: u32| (header.entries_offset + slot * header.entry_bytes + 40) as usize / 4;

    // A define under way: odd. What was read stays.
    // SAFETY: inside the words, as above.
    unsafe { *base.add(GENERATION_WORD) = 3 };
    assert!(!r.refresh());
    assert_eq!(r.generation(), 1);
    assert_eq!(r.find(1).unwrap().name(), "cutoff");

    // Landed: even, and new.
    // SAFETY: inside the words, as above.
    unsafe {
        *base.add(name_at(entries)) = u32::from_le_bytes(*b"new\0");
        *base.add(GENERATION_WORD) = 4;
    }
    assert!(r.refresh());
    assert_eq!(r.generation(), 2);
    assert_eq!(r.find(1).unwrap().name(), "new");
}

#[test]
fn a_value_is_read_from_memory_each_time() {
    let mut words = fixture();
    let base = words.as_mut_ptr();
    // SAFETY: the words outlive the reader, and this test is the only thread.
    let mut r = unsafe { Reader::from_raw(base.cast::<u8>().cast_const(), words.len() * 4) }.unwrap();
    r.refresh();
    let slot = r.find(1).unwrap().slot();
    assert_eq!(r.value(slot), 440.0);
    // SAFETY: the values start at the header's offset, within the words.
    unsafe { *base.add(16 + slot as usize) = 880.0f32.to_bits() };
    assert_eq!(r.value(slot), 880.0, "no refresh, and no copy");
    assert!(!r.refresh(), "a value moving is not the set of entries moving");
    assert_eq!(r.value(u32::MAX), 0.0, "outside the table");
}

#[test]
fn bytes_that_are_no_table_are_refused() {
    let words = fixture();
    assert!(Reader::new(&[]).is_none());
    assert!(Reader::new(&bytes_of(&words)[..32]).is_none(), "shorter than a header");
    assert!(Reader::new(&bytes_of(&words)[1..]).is_none(), "not on a word");
    assert!(Reader::new(&bytes_of(&words)[..100]).is_none(), "shorter than its entries");
    let mut other = words.clone();
    other[1] = 1;
    assert!(Reader::new(bytes_of(&other)).is_none(), "another version");
    let mut other = words.clone();
    other[0] = 0;
    assert!(Reader::new(bytes_of(&other)).is_none(), "no magic");
    let mut other = words.clone();
    other[7] = 12;
    assert!(Reader::new(bytes_of(&other)).is_none(), "entries too small to hold one");
}

#[test]
fn text_that_points_outside_the_region_reads_as_none() {
    let mut words = fixture();
    // SAFETY: the fixture starts with a header, and the words lie on a word.
    let header = unsafe { core::ptr::read(words.as_ptr().cast::<ClockworkParamsHeader>()) };
    // Slot 1 is cutoff: its text word, at +80 in the entry, sent far away.
    let at = (header.entries_offset + header.entry_bytes + 80) as usize / 4;
    words[at] = 0xFFFF_FFF0;
    let mut r = Reader::new(bytes_of(&words)).unwrap();
    r.refresh();
    let cutoff = r.find(1).unwrap();
    assert_eq!(cutoff.doc(), None);
    assert_eq!(cutoff.full_name(), "cutoff");
}

/// A writer on another thread follows the region's protocol, odd then even, as a
/// define does, and every read belongs to one write.
#[test]
fn a_reader_on_another_thread_never_sees_one_write_mixed_with_another() {
    let words = fixture();
    // SAFETY: the fixture starts with a header, and the words lie on a word.
    let header = unsafe { core::ptr::read(words.as_ptr().cast::<ClockworkParamsHeader>()) };
    let shared: Vec<AtomicU32> = words.iter().map(|&w| AtomicU32::new(w)).collect();
    let min_word = (header.entries_offset + 24) as usize / 4;
    let max_word = min_word + 1;
    let stop = AtomicBool::new(false);
    let refreshes = AtomicU64::new(0);
    let shared_ref = &shared;
    std::thread::scope(|s| {
        s.spawn(|| {
            let bytes = shared_ref.len() * 4;
            // SAFETY: the atomics outlive the scope, and the reader only loads.
            let mut r = unsafe { Reader::from_raw(shared_ref.as_ptr().cast::<u8>(), bytes) }.unwrap();
            while !stop.load(Ordering::Relaxed) {
                if r.refresh() {
                    refreshes.fetch_add(1, Ordering::Relaxed);
                    let e = r.params()[0].entry();
                    assert_eq!(e.max - e.min, 1.0, "one write's min with another's max");
                }
            }
        });
        // Bursts, so reads overlap writes, with gaps, so reads also finish: a writer
        // that never paused would keep every reader reading again, as it should.
        let mut round = 0u32;
        while round < 200_000 && refreshes.load(Ordering::Relaxed) < 2_000 {
            round += 1;
            if round % 8 == 0 {
                std::thread::sleep(std::time::Duration::from_micros(20));
            }
            let g = shared[GENERATION_WORD].load(Ordering::Relaxed);
            shared[GENERATION_WORD].store(g + 1, Ordering::Relaxed);
            std::sync::atomic::fence(Ordering::Release);
            let f = (round % 1000) as f32;
            shared[min_word].store(f.to_bits(), Ordering::Relaxed);
            shared[max_word].store((f + 1.0).to_bits(), Ordering::Relaxed);
            shared[GENERATION_WORD].store(g + 2, Ordering::Release);
        }
        stop.store(true, Ordering::Relaxed);
    });
    assert!(refreshes.load(Ordering::Relaxed) >= 100, "the reader did look while the table moved");
}

#[test]
fn a_define_lays_out_entries_and_their_text() {
    let mut d = Define::new();
    d.add(ClockworkParamEntry::new(1, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.0, 1.0, 0.0), &[]);
    d.add(ClockworkParamEntry::new(2, "b", CLOCKWORK_PARAM_CHOICE, 0.0, 1.0, 0.0),
          &[Say::Doc("what"), Say::Value("x"), Say::Value("y")]);
    d.add(ClockworkParamEntry::new(3, "c", CLOCKWORK_PARAM_CONTINUOUS, 0.0, 1.0, 0.0), &[Say::FullName("long c")]);
    assert_eq!(d.text(), b"\x02what\0\x04x\0\x04y\0\x01long c\0");
    let e = d.entries();
    assert_eq!((e[0].text, e[0].text_bytes), (0, 0));
    assert_eq!((e[1].text, e[1].text_bytes), (0, 12));
    assert_eq!((e[2].text, e[2].text_bytes), (12, 8));
}

#[test]
fn a_name_is_cut_on_a_characters_edge() {
    let e = ClockworkParamEntry::new(1, &"é".repeat(40), CLOCKWORK_PARAM_CONTINUOUS, 0.0, 1.0, 0.0);
    assert_eq!(e.name_str(), "é".repeat(15), "31 bytes would split the sixteenth");
}
