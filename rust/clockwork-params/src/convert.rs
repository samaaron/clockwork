// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! What a value means, from the facts alone, so that every bridge and panel says
//! the same: where it sits on a knob, and how it reads.
use clockwork_abi::params::*;

use crate::reader::Param;

impl Param {
    /// A value as the table would hold it: within the range, and whole for the
    /// integer kinds. What the engine does to every set (`held` in params.cpp).
    pub fn held(&self, v: f32) -> f32 {
        let e = &self.entry;
        let v = if v.is_finite() { v } else { e.min };
        let v = if e.kind == CLOCKWORK_PARAM_CONTINUOUS { v } else { v.round() };
        if v < e.min { e.min } else if v > e.max { e.max } else { v }
    }

    /// Where a value sits on a knob, 0 to 1, by the entry's taper.
    pub fn position_of(&self, v: f32) -> f32 {
        let e = &self.entry;
        if e.max <= e.min || !v.is_finite() {
            return 0.0;
        }
        let v = f64::from(self.held(v));
        let (min, max) = (f64::from(e.min), f64::from(e.max));
        let p = if e.taper == CLOCKWORK_PARAM_TAPER_LOG && min > 0.0 {
            (v / min).ln() / (max / min).ln()
        } else if e.taper > 0.0 {
            ((v - min) / (max - min)).powf(f64::from(e.taper))
        } else {
            (v - min) / (max - min)
        };
        p.clamp(0.0, 1.0) as f32
    }

    /// The value at a place on a knob, held as the table would hold it.
    pub fn value_at(&self, position: f32) -> f32 {
        let e = &self.entry;
        if e.max <= e.min {
            return e.min;
        }
        let p = if position.is_finite() { f64::from(position).clamp(0.0, 1.0) } else { 0.0 };
        let (min, max) = (f64::from(e.min), f64::from(e.max));
        let v = if e.taper == CLOCKWORK_PARAM_TAPER_LOG && min > 0.0 {
            min * (max / min).powf(p)
        } else if e.taper > 0.0 {
            min + p.powf(1.0 / f64::from(e.taper)) * (max - min)
        } else {
            min + p * (max - min)
        };
        self.held(v as f32)
    }

    /// A value in words. Empty for a group, which holds none.
    pub fn text_of(&self, v: f32) -> String {
        let e = &self.entry;
        let held = self.held(v);
        match e.kind {
            CLOCKWORK_PARAM_GROUP => String::new(),
            CLOCKWORK_PARAM_SWITCH => if held >= 0.5 { "On" } else { "Off" }.to_string(),
            CLOCKWORK_PARAM_NOTE => note_name(held as i32),
            CLOCKWORK_PARAM_CHOICE => self.value_name(held as u32).map_or_else(|| fixed(f64::from(held), 0), str::to_string),
            _ => self.number_text(f64::from(held)),
        }
    }

    fn number_text(&self, v: f64) -> String {
        let e = &self.entry;
        let places = self.places();
        match e.unit {
            CLOCKWORK_PARAM_UNIT_PERCENT => format!("{}%", fixed(v, 0)),
            CLOCKWORK_PARAM_UNIT_DECIBELS if self.floor_is_off() && fixed(v, 1).parse::<f64>().is_ok_and(|shown| shown <= f64::from(e.min)) => "Off".to_string(),
            CLOCKWORK_PARAM_UNIT_DECIBELS => format!("{} dB", fixed(v, 1)),
            CLOCKWORK_PARAM_UNIT_HERTZ if v.round() >= 1000.0 => format!("{} kHz", fixed(v / 1000.0, 2)),
            CLOCKWORK_PARAM_UNIT_HERTZ => format!("{} Hz", fixed(v, 0)),
            CLOCKWORK_PARAM_UNIT_SECONDS if (v * 1000.0).round() >= 1000.0 => format!("{} s", fixed(v, 2)),
            CLOCKWORK_PARAM_UNIT_SECONDS => format!("{} ms", fixed(v * 1000.0, 0)),
            CLOCKWORK_PARAM_UNIT_BPM => format!("{} BPM", fixed(v, 0)),
            CLOCKWORK_PARAM_UNIT_SEMITONES => {
                let n = fixed(v, 0);
                if n.starts_with('-') { format!("{n} st") } else { format!("+{n} st") }
            }
            CLOCKWORK_PARAM_UNIT_CUSTOM => format!("{} {}", fixed(v, places), self.unit_text().unwrap_or("")),
            _ => fixed(v, places),
        }
    }

    // Decimals enough to tell a small move from none, whatever the range is of:
    // 0 to 1 shows 0.25, 0 to 0.01 shows 0.0030, 0 to 5000 shows 1201.
    fn places(&self) -> usize {
        let e = &self.entry;
        let span = f64::from(e.max) - f64::from(e.min);
        if e.kind != CLOCKWORK_PARAM_CONTINUOUS || span <= 0.0 {
            return 0;
        }
        // The nudge is for a span a float cannot quite say: 0.01 arrives as 0.00999...
        (2.0 - (span.log10() + 1e-6).floor()).clamp(0.0, 6.0) as usize
    }

    // A level that bottoms out at -60 dB or below is silent there; a trim that
    // stops at -6 is not.
    fn floor_is_off(&self) -> bool { self.entry.min <= -60.0 }

    /// What someone typed, or a host passed, as a value held as the table would
    /// hold it. None when the words are not one this entry can have.
    pub fn value_of_text(&self, words: &str) -> Option<f32> {
        let words = words.trim();
        let e = &self.entry;
        let v = match e.kind {
            CLOCKWORK_PARAM_GROUP => None,
            CLOCKWORK_PARAM_SWITCH => match words.to_ascii_lowercase().as_str() {
                "on" | "true" | "yes" => Some(1.0),
                "off" | "false" | "no" => Some(0.0),
                _ => bare_number(words),
            },
            CLOCKWORK_PARAM_NOTE => note_number(words).or_else(|| bare_number(words)),
            CLOCKWORK_PARAM_CHOICE => self.value_names()
                .position(|name| name.eq_ignore_ascii_case(words))
                .map(|index| index as f64)
                .or_else(|| bare_number(words)),
            _ => self.number_of_text(words),
        }?;
        Some(self.held(v as f32))
    }

    fn number_of_text(&self, words: &str) -> Option<f64> {
        let e = &self.entry;
        if e.unit == CLOCKWORK_PARAM_UNIT_DECIBELS && self.floor_is_off() && words.eq_ignore_ascii_case("off") {
            return Some(f64::from(e.min));
        }
        let (n, rest) = number_prefix(words)?;
        let is = |said: &str| rest.eq_ignore_ascii_case(said);
        let bare = rest.is_empty();
        match e.unit {
            CLOCKWORK_PARAM_UNIT_PERCENT if bare || is("%") => Some(n),
            CLOCKWORK_PARAM_UNIT_DECIBELS if bare || is("dB") => Some(n),
            CLOCKWORK_PARAM_UNIT_HERTZ if bare || is("Hz") => Some(n),
            CLOCKWORK_PARAM_UNIT_HERTZ if is("kHz") => Some(n * 1000.0),
            CLOCKWORK_PARAM_UNIT_SECONDS if bare || is("s") => Some(n),
            CLOCKWORK_PARAM_UNIT_SECONDS if is("ms") => Some(n / 1000.0),
            CLOCKWORK_PARAM_UNIT_BPM if bare || is("BPM") => Some(n),
            CLOCKWORK_PARAM_UNIT_SEMITONES if bare || is("st") => Some(n),
            CLOCKWORK_PARAM_UNIT_CUSTOM if bare || self.unit_text().is_some_and(is) => Some(n),
            CLOCKWORK_PARAM_UNIT_NONE if bare => Some(n),
            _ => None,
        }
    }
}

// A number to so many places, with no minus on what shows as zero.
fn fixed(v: f64, places: usize) -> String {
    let s = format!("{v:.places$}");
    match s.strip_prefix('-') {
        Some(rest) if rest.bytes().all(|b| b == b'0' || b == b'.') => rest.to_string(),
        _ => s,
    }
}

// Middle C, MIDI note 60, is C4.
fn note_name(note: i32) -> String {
    const NAMES: [&str; 12] = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    format!("{}{}", NAMES[note.rem_euclid(12) as usize], note.div_euclid(12) - 1)
}

fn note_number(words: &str) -> Option<f64> {
    let mut chars = words.chars();
    let semitone = match chars.next()?.to_ascii_uppercase() {
        'C' => 0, 'D' => 2, 'E' => 4, 'F' => 5, 'G' => 7, 'A' => 9, 'B' => 11,
        _ => return None,
    };
    let rest = chars.as_str();
    let (shift, octave) = match rest.as_bytes().first()? {
        b'#' => (1, &rest[1..]),
        b'b' | b'B' => (-1, &rest[1..]),
        _ => (0, rest),
    };
    let octave: i32 = octave.parse().ok()?;
    Some(f64::from(octave.checked_add(1)?.checked_mul(12)?.checked_add(semitone + shift)?))
}

fn bare_number(words: &str) -> Option<f64> {
    number_prefix(words).and_then(|(n, rest)| rest.is_empty().then_some(n))
}

// A plain decimal at the front, and what follows it: no exponent, no "inf" or
// "NaN", which are not how anyone writes a parameter's value.
fn number_prefix(words: &str) -> Option<(f64, &str)> {
    let b = words.as_bytes();
    let mut end = usize::from(matches!(b.first(), Some(b'+' | b'-')));
    let digits_from = end;
    while b.get(end).is_some_and(u8::is_ascii_digit) { end += 1; }
    let mut digits = end - digits_from;
    if b.get(end) == Some(&b'.') {
        end += 1;
        let after = end;
        while b.get(end).is_some_and(u8::is_ascii_digit) { end += 1; }
        digits += end - after;
    }
    if digits == 0 {
        return None;
    }
    let n: f64 = words[..end].parse().ok()?;
    Some((n, words[end..].trim()))
}
