// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! What a value means: the one implementation every bridge and panel calls.
use clockwork_params::*;

fn param(e: ClockworkParamEntry, says: &[Say]) -> Param {
    let mut d = Define::new();
    d.add(e, says);
    let entry = d.entries()[0];
    // SAFETY: any entry is its bytes.
    let bytes = unsafe { core::slice::from_raw_parts((&raw const entry).cast::<u8>(), core::mem::size_of_val(&entry)) };
    Param::from_bytes(bytes, d.text()).expect("an entry's bytes")
}
fn plain(kind: u32, min: f32, max: f32, unit: u32) -> Param {
    param(ClockworkParamEntry::new(1, "p", kind, min, max, min).unit(unit), &[])
}
const C: u32 = CLOCKWORK_PARAM_CONTINUOUS;
const NONE: u32 = CLOCKWORK_PARAM_UNIT_NONE;

fn near(a: f32, b: f32, within: f32) {
    assert!((a - b).abs() <= within, "{a} is not within {within} of {b}");
}

#[test]
fn a_linear_entry_sweeps_evenly() {
    let p = plain(C, -60.0, 0.0, CLOCKWORK_PARAM_UNIT_DECIBELS);
    near(p.position_of(-60.0), 0.0, 1e-6);
    near(p.position_of(-30.0), 0.5, 1e-6);
    near(p.position_of(0.0), 1.0, 1e-6);
    near(p.value_at(0.25), -45.0, 1e-4);
}

#[test]
fn a_log_taper_gives_each_octave_the_same_travel() {
    let p = param(ClockworkParamEntry::new(1, "f", C, 40.0, 12000.0, 800.0).taper(CLOCKWORK_PARAM_TAPER_LOG), &[]);
    near(p.value_at(0.5), 692.8, 0.1);
    near(p.position_of(692.8), 0.5, 1e-4);
    near(p.position_of(40.0), 0.0, 1e-6);
    near(p.position_of(12000.0), 1.0, 1e-6);
}

#[test]
fn a_power_taper_is_the_formula_the_header_states() {
    let p = param(ClockworkParamEntry::new(1, "d", C, 0.0, 4.0, 0.0).taper(0.5), &[]);
    near(p.position_of(1.0), 0.25f32.powf(0.5), 1e-6);
    near(p.value_at(0.5), 1.0, 1e-5);
}

#[test]
fn the_position_and_the_value_undo_each_other() {
    let tapers = [CLOCKWORK_PARAM_TAPER_LINEAR, CLOCKWORK_PARAM_TAPER_LOG, 0.3, 0.5, 2.0, 3.0];
    for taper in tapers {
        let p = param(ClockworkParamEntry::new(1, "t", C, 0.02, 2000.0, 1.0).taper(taper), &[]);
        for i in 0..=1000 {
            let pos = i as f32 / 1000.0;
            near(p.position_of(p.value_at(pos)), pos, 2e-4);
        }
    }
}

#[test]
fn a_position_or_a_value_outside_is_held_at_the_edge() {
    let p = plain(C, 0.0, 10.0, NONE);
    assert_eq!(p.position_of(-5.0), 0.0);
    assert_eq!(p.position_of(50.0), 1.0);
    assert_eq!(p.position_of(f32::NAN), 0.0);
    assert_eq!(p.value_at(-1.0), 0.0);
    assert_eq!(p.value_at(2.0), 10.0);
    assert_eq!(p.value_at(f32::NAN), 0.0);
}

#[test]
fn an_integer_kind_lands_on_whole_values() {
    let s = plain(CLOCKWORK_PARAM_STEPPED, 1.0, 4.0, NONE);
    assert_eq!(s.value_at(0.0), 1.0);
    assert_eq!(s.value_at(0.49), 2.0);
    assert_eq!(s.value_at(1.0), 4.0);
    assert_eq!(s.held(2.6), 3.0);
    let sw = plain(CLOCKWORK_PARAM_SWITCH, 0.0, 1.0, NONE);
    assert_eq!(sw.value_at(0.4), 0.0);
    assert_eq!(sw.value_at(0.6), 1.0);
}

#[test]
fn a_group_has_no_place_and_no_words() {
    let g = param(ClockworkParamEntry::new_group(1, "g"), &[]);
    assert_eq!(g.position_of(0.5), 0.0);
    assert_eq!(g.value_at(0.5), 0.0);
    assert_eq!(g.text_of(0.5), "");
    assert_eq!(g.value_of_text("anything"), None);
}

#[test]
fn a_value_says_itself_in_its_units_words() {
    let u = |unit| plain(C, -100.0, 20000.0, unit);
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_PERCENT).text_of(30.0), "30%");
    // A percent is in percent whatever its range: a pan from -100 to 100, a boost to 200.
    assert_eq!(plain(C, -100.0, 100.0, CLOCKWORK_PARAM_UNIT_PERCENT).text_of(-50.0), "-50%");
    assert_eq!(plain(C, 0.0, 200.0, CLOCKWORK_PARAM_UNIT_PERCENT).text_of(150.0), "150%");
    assert_eq!(plain(C, 0.0, 200.0, CLOCKWORK_PARAM_UNIT_PERCENT).value_of_text("150%"), Some(150.0));
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_DECIBELS).text_of(-12.0), "-12.0 dB");
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_HERTZ).text_of(800.0), "800 Hz");
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_HERTZ).text_of(1800.0), "1.80 kHz");
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_SECONDS).text_of(0.25), "250 ms");
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_SECONDS).text_of(1.5), "1.50 s");
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_BPM).text_of(120.0), "120 BPM");
    assert_eq!(u(CLOCKWORK_PARAM_UNIT_SEMITONES).text_of(3.0), "+3 st");
    assert_eq!(plain(C, 0.0, 1.0, NONE).text_of(0.25), "0.25");
    let custom = param(ClockworkParamEntry::new(1, "c", C, 0.0, 9.0, 0.0).unit(CLOCKWORK_PARAM_UNIT_CUSTOM), &[Say::Unit("beats")]);
    assert_eq!(custom.text_of(1.5), "1.50 beats");
    let stepped = param(ClockworkParamEntry::new(1, "c", CLOCKWORK_PARAM_STEPPED, 0.0, 9.0, 0.0).unit(CLOCKWORK_PARAM_UNIT_CUSTOM), &[Say::Unit("beats")]);
    assert_eq!(stepped.text_of(3.0), "3 beats");
    assert_eq!(plain(CLOCKWORK_PARAM_STEPPED, 1.0, 4.0, NONE).text_of(2.0), "2");
    assert_eq!(plain(CLOCKWORK_PARAM_STEPPED, -3.0, 3.0, CLOCKWORK_PARAM_UNIT_SEMITONES).text_of(-2.0), "-2 st");
}

#[test]
fn the_floor_of_a_level_is_off_and_a_trims_is_not() {
    let level = plain(C, -60.0, 0.0, CLOCKWORK_PARAM_UNIT_DECIBELS);
    assert_eq!(level.text_of(-60.0), "Off");
    assert_eq!(level.value_of_text("off"), Some(-60.0));
    let trim = plain(C, -6.0, 6.0, CLOCKWORK_PARAM_UNIT_DECIBELS);
    assert_eq!(trim.text_of(-6.0), "-6.0 dB");
    assert_eq!(trim.value_of_text("off"), None);
}

#[test]
fn a_switch_a_note_and_a_choice_have_their_own_words() {
    let sw = plain(CLOCKWORK_PARAM_SWITCH, 0.0, 1.0, NONE);
    assert_eq!(sw.text_of(1.0), "On");
    assert_eq!(sw.text_of(0.0), "Off");
    let note = plain(CLOCKWORK_PARAM_NOTE, 0.0, 127.0, NONE);
    assert_eq!(note.text_of(60.0), "C4");
    assert_eq!(note.text_of(45.0), "A2");
    assert_eq!(note.text_of(0.0), "C-1");
    let choice = param(ClockworkParamEntry::new(1, "w", CLOCKWORK_PARAM_CHOICE, 0.0, 2.0, 0.0),
                       &[Say::Value("Saw"), Say::Value("Square"), Say::Value("Tri")]);
    assert_eq!(choice.text_of(1.0), "Square");
    assert_eq!(choice.text_of(1.6), "Tri");
}

#[test]
fn words_come_back_as_values() {
    let hz = plain(C, 20.0, 20000.0, CLOCKWORK_PARAM_UNIT_HERTZ);
    assert_eq!(hz.value_of_text("1.2 kHz"), Some(1200.0));
    assert_eq!(hz.value_of_text("800 Hz"), Some(800.0));
    assert_eq!(hz.value_of_text("  800hz "), Some(800.0));
    assert_eq!(hz.value_of_text("800"), Some(800.0));
    assert_eq!(hz.value_of_text("5"), Some(20.0), "held to the range");
    assert_eq!(hz.value_of_text("800 dB"), None, "another unit's words");
    assert_eq!(hz.value_of_text("loud"), None);
    assert_eq!(hz.value_of_text(""), None);
    assert_eq!(hz.value_of_text("-"), None);
    assert_eq!(hz.value_of_text("NaN"), None);
    assert_eq!(hz.value_of_text("inf"), None);
    let db = plain(C, -60.0, 6.0, CLOCKWORK_PARAM_UNIT_DECIBELS);
    assert_eq!(db.value_of_text("-6 dB"), Some(-6.0));
    assert_eq!(plain(C, 0.0, 100.0, CLOCKWORK_PARAM_UNIT_PERCENT).value_of_text("50%"), Some(50.0));
    let s = plain(C, 0.0, 10.0, CLOCKWORK_PARAM_UNIT_SECONDS);
    assert_eq!(s.value_of_text("250 ms"), Some(0.25));
    assert_eq!(s.value_of_text("1.5 s"), Some(1.5));
    assert_eq!(plain(C, 40.0, 240.0, CLOCKWORK_PARAM_UNIT_BPM).value_of_text("120 bpm"), Some(120.0));
    assert_eq!(plain(CLOCKWORK_PARAM_STEPPED, -12.0, 12.0, CLOCKWORK_PARAM_UNIT_SEMITONES).value_of_text("+3 st"), Some(3.0));
    let custom = param(ClockworkParamEntry::new(1, "c", C, 0.0, 9.0, 0.0).unit(CLOCKWORK_PARAM_UNIT_CUSTOM), &[Say::Unit("Beats")]);
    assert_eq!(custom.value_of_text("2 beats"), Some(2.0));
    assert_eq!(custom.value_of_text("2 bars"), None);

    let sw = plain(CLOCKWORK_PARAM_SWITCH, 0.0, 1.0, NONE);
    assert_eq!(sw.value_of_text("on"), Some(1.0));
    assert_eq!(sw.value_of_text("OFF"), Some(0.0));
    assert_eq!(sw.value_of_text("1"), Some(1.0));
    assert_eq!(sw.value_of_text("maybe"), None);

    let note = plain(CLOCKWORK_PARAM_NOTE, 0.0, 127.0, NONE);
    assert_eq!(note.value_of_text("C4"), Some(60.0));
    assert_eq!(note.value_of_text("f#3"), Some(54.0));
    assert_eq!(note.value_of_text("Db2"), Some(37.0));
    assert_eq!(note.value_of_text("C-1"), Some(0.0));
    assert_eq!(note.value_of_text("60"), Some(60.0));
    assert_eq!(note.value_of_text("H2"), None);
    assert_eq!(note.value_of_text("C"), None);
    assert_eq!(note.value_of_text("C99"), Some(127.0), "held to the range");

    let choice = param(ClockworkParamEntry::new(1, "w", CLOCKWORK_PARAM_CHOICE, 0.0, 1.0, 0.0), &[Say::Value("Saw"), Say::Value("Tri")]);
    assert_eq!(choice.value_of_text("tri"), Some(1.0));
    assert_eq!(choice.value_of_text("0"), Some(0.0));
    assert_eq!(choice.value_of_text("9"), Some(1.0), "held to the range");
    assert_eq!(choice.value_of_text("sine"), None);
}

/// Whatever is shown can be typed back, and shows the same.
#[test]
fn every_value_shown_reads_back_as_itself() {
    let params = [
        plain(C, -60.0, 6.0, CLOCKWORK_PARAM_UNIT_DECIBELS),
        plain(C, 20.0, 20000.0, CLOCKWORK_PARAM_UNIT_HERTZ),
        plain(C, 0.0, 100.0, CLOCKWORK_PARAM_UNIT_PERCENT),
        plain(C, 0.01, 10.0, CLOCKWORK_PARAM_UNIT_SECONDS),
        plain(C, 40.0, 240.0, CLOCKWORK_PARAM_UNIT_BPM),
        plain(C, -24.0, 24.0, CLOCKWORK_PARAM_UNIT_SEMITONES),
        plain(C, -1.0, 1.0, NONE),
        plain(CLOCKWORK_PARAM_STEPPED, -8.0, 8.0, NONE),
        plain(CLOCKWORK_PARAM_SWITCH, 0.0, 1.0, NONE),
        plain(CLOCKWORK_PARAM_NOTE, 0.0, 127.0, NONE),
        param(ClockworkParamEntry::new(1, "c", C, 0.0, 9.0, 0.0).unit(CLOCKWORK_PARAM_UNIT_CUSTOM), &[Say::Unit("beats a bar")]),
        param(ClockworkParamEntry::new(1, "w", CLOCKWORK_PARAM_CHOICE, 0.0, 2.0, 0.0), &[Say::Value("Saw"), Say::Value("Square"), Say::Value("é è")]),
    ];
    for p in &params {
        for i in 0..=997 {
            let v = p.value_at(i as f32 / 997.0);
            let shown = p.text_of(v);
            let back = p.value_of_text(&shown).unwrap_or_else(|| panic!("{shown:?} did not read back"));
            assert_eq!(p.text_of(back), shown, "{shown:?} read back as {back}");
        }
    }
}

/// The table says nothing about music: these are a machine's, not an instrument's.
#[test]
fn a_table_of_anything_says_itself_in_its_own_words() {
    let custom = |min, max, unit: &str| param(ClockworkParamEntry::new(1, "p", C, min, max, min).unit(CLOCKWORK_PARAM_UNIT_CUSTOM), &[Say::Unit(unit)]);
    let temperature = custom(-40.0, 125.0, "°C");
    assert_eq!(temperature.text_of(21.5), "22 °C", "a range of hundreds shows whole degrees");
    assert_eq!(temperature.value_of_text("22 °C"), Some(22.0));
    let volts = custom(0.0, 0.01, "V");
    assert_eq!(volts.text_of(0.003), "0.0030 V", "a small range keeps its small moves");
    assert_eq!(volts.value_of_text("0.0030 V"), Some(0.003));
    let speed = custom(0.0, 5000.0, "rpm");
    assert_eq!(speed.text_of(1200.7), "1201 rpm");
    let bare = plain(C, 0.0, 1.0, NONE);
    assert_eq!(bare.text_of(0.25), "0.25");
    let angle = param(ClockworkParamEntry::new(1, "a", C, 0.0, 360.0, 0.0).unit(CLOCKWORK_PARAM_UNIT_CUSTOM).flags(CLOCKWORK_PARAM_WRAPS), &[Say::Unit("°")]);
    assert_eq!(angle.text_of(90.0), "90 °");
    let mode = param(ClockworkParamEntry::new(1, "m", CLOCKWORK_PARAM_CHOICE, 0.0, 2.0, 0.0), &[Say::Value("Idle"), Say::Value("Run"), Say::Value("Fault")]);
    assert_eq!(mode.text_of(2.0), "Fault");
    assert_eq!(mode.value_of_text("run"), Some(1.0));
    for p in [&temperature, &volts, &speed, &angle] {
        for i in 0..=997 {
            let shown = p.text_of(p.value_at(i as f32 / 997.0));
            let back = p.value_of_text(&shown).unwrap_or_else(|| panic!("{shown:?} did not read back"));
            assert_eq!(p.text_of(back), shown);
        }
    }
}

#[test]
fn bytes_that_are_no_entry_are_refused() {
    assert!(Param::from_bytes(&[0; 12], &[]).is_none());
}
