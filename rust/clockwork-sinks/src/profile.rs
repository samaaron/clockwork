// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Size classes: what a sink of each kind is made of.
//!
//! A queue's cells are one width (see [`crate::queue`]), and one width is
//! wrong for every kind of message there is. Nearly every MIDI message is
//! three bytes and a sysex dump is kilobytes; nearly every OSC message is
//! under a hundred bytes and a datagram may carry sixty-four thousand. A
//! sink is therefore SEVERAL queues, one per class, and a message takes the
//! narrowest cell it fits. The classes are a property of the KIND, decided
//! by the host at boot from its memory profile, so a DSP opening a sink gets
//! the right shape without knowing there is one.
//!
//! # Memory
//!
//! What a class costs is `cells × cell_bytes` of payload, reserved at open.
//! The defaults below are the desktop's: a quarter of a megabyte for an OSC
//! endpoint, a fraction of that for a MIDI port. An embedded profile shrinks
//! or drops the wide classes; `memory_profile.h` owns the numbers.

use std::sync::Mutex;

use crate::queue::{round_capacity, DEFAULT_CELL_BYTES};
use crate::sink::{KIND_MIDI, KIND_OSC};

/// The shallowest class there is. The queue underneath tells "full" from
/// "empty" by a cell's sequence number, and with a single cell the two read
/// the same — so a class is two cells at least, and a profile that asks for
/// one gets two.
pub const MIN_CLASS_DEPTH: u32 = 2;

/// One class: how wide a cell is, and how many there are.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Class {
    pub cell_bytes: u32,
    pub cells: u32,
}

/// A sink's shape: its classes, narrowest first, each width once.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Layout {
    classes: Vec<Class>,
}

impl Layout {
    /// The one-class shape: what every sink was before profiles.
    pub fn single(cell_bytes: u32, cells: u32) -> Layout {
        Layout::normalise(vec![Class { cell_bytes, cells }]).expect("one non-empty class")
    }

    /// Order the classes, round each depth to a power of two of at least
    /// [`MIN_CLASS_DEPTH`], and refuse a shape that is empty or names one
    /// width twice — two classes of the same width are one class with a
    /// confusing count, not a layout.
    pub fn normalise(mut classes: Vec<Class>) -> Option<Layout> {
        classes.retain(|c| c.cell_bytes > 0 && c.cells > 0);
        if classes.is_empty() { return None; }
        classes.sort_by_key(|c| c.cell_bytes);
        if classes.windows(2).any(|w| w[0].cell_bytes == w[1].cell_bytes) { return None; }
        for c in &mut classes {
            let d = c.cells.max(MIN_CLASS_DEPTH);
            c.cells = if d.is_power_of_two() { d } else { d.next_power_of_two() };
        }
        Some(Layout { classes })
    }

    pub fn classes(&self) -> &[Class] { &self.classes }

    /// The narrowest class: the one `capacity` at open sizes.
    pub fn base(&self) -> Class { self.classes[0] }

    pub fn largest_cell_bytes(&self) -> u32 {
        self.classes.last().map(|c| c.cell_bytes).unwrap_or(0)
    }

    /// Payload bytes the shape reserves.
    pub fn bytes_reserved(&self) -> u64 {
        self.classes.iter().map(|c| c.cells as u64 * c.cell_bytes as u64).sum()
    }

    /// The same shape with the base class `capacity` deep — the meaning of
    /// `clockwork_sink_open`'s argument. 0 keeps the profile's own depth.
    pub fn with_base_depth(&self, capacity: u32) -> Layout {
        if capacity == 0 { return self.clone(); }
        let mut classes = self.classes.clone();
        classes[0].cells = round_capacity(capacity);
        Layout { classes }
    }
}

/// The desktop defaults, per kind. `memory_profile.h` carries the same
/// numbers as the strings the host parses; these are what a build that never
/// calls `clockwork_sink_profile` gets.
pub fn default_for(kind: u32) -> Layout {
    let classes = match kind {
        // Sixteen ordinary cells, then two of each size up to a whole datagram.
        KIND_OSC => vec![
            Class { cell_bytes: 1024, cells: 16 },
            Class { cell_bytes: 8192, cells: 2 },
            Class { cell_bytes: 16384, cells: 2 },
            Class { cell_bytes: 32768, cells: 2 },
            Class { cell_bytes: 65536, cells: 2 },
        ],
        // Every channel and realtime message is three bytes or fewer; only
        // sysex is wider, and rare. Two wide cells because a class cannot be
        // one (MIN_CLASS_DEPTH), and it lets a second dump queue behind the
        // first.
        KIND_MIDI => vec![
            Class { cell_bytes: 16, cells: 256 },
            Class { cell_bytes: 65536, cells: 2 },
        ],
        _ => vec![Class { cell_bytes: DEFAULT_CELL_BYTES as u32, cells: 8 }],
    };
    Layout::normalise(classes).expect("the defaults are well formed")
}

/// Kind → the layout a host installed. Consulted at OPEN only, which is a
/// control-thread call, so a lock here costs the audio thread nothing.
static INSTALLED: Mutex<Vec<(u32, Layout)>> = Mutex::new(Vec::new());

/// Install a shape for a kind. Returns false for a shape that does not
/// normalise; the previous shape (or the default) then stays.
pub fn install(kind: u32, classes: Vec<Class>) -> bool {
    let Some(layout) = Layout::normalise(classes) else { return false };
    let Ok(mut installed) = INSTALLED.lock() else { return false };
    installed.retain(|(k, _)| *k != kind);
    installed.push((kind, layout));
    true
}

/// The shape a sink of `kind` is built to: installed, else the default.
pub fn for_kind(kind: u32) -> Layout {
    if let Ok(installed) = INSTALLED.lock() {
        if let Some((_, l)) = installed.iter().find(|(k, _)| *k == kind) {
            return l.clone();
        }
    }
    default_for(kind)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_layout_is_sorted_rounded_and_rejects_a_repeated_width() {
        let l = Layout::normalise(vec![
            Class { cell_bytes: 65536, cells: 1 },
            Class { cell_bytes: 16, cells: 200 },
            Class { cell_bytes: 8192, cells: 3 },
        ]).expect("well formed");
        let widths: Vec<u32> = l.classes().iter().map(|c| c.cell_bytes).collect();
        assert_eq!(widths, vec![16, 8192, 65536], "narrowest first");
        let depths: Vec<u32> = l.classes().iter().map(|c| c.cells).collect();
        assert_eq!(depths, vec![256, 4, 2], "each depth a power of two, and two at least");
        assert_eq!(l.largest_cell_bytes(), 65536);
        assert_eq!(l.bytes_reserved(), 256 * 16 + 4 * 8192 + 2 * 65536);

        assert!(Layout::normalise(vec![]).is_none(), "no classes is no shape");
        assert!(Layout::normalise(vec![Class { cell_bytes: 64, cells: 0 }]).is_none(),
                "a class with no cells is no class");
        assert!(Layout::normalise(vec![
            Class { cell_bytes: 64, cells: 2 }, Class { cell_bytes: 64, cells: 4 },
        ]).is_none(), "one width twice is refused, not summed");
    }

    #[test]
    fn the_desktop_defaults_are_what_the_profile_header_says() {
        let osc = default_for(KIND_OSC);
        assert_eq!(osc.bytes_reserved(), 256 * 1024, "a quarter of a megabyte per OSC endpoint");
        assert_eq!(osc.largest_cell_bytes(), 65536, "a whole datagram");
        assert_eq!(osc.base(), Class { cell_bytes: 1024, cells: 16 });

        let midi = default_for(KIND_MIDI);
        assert_eq!(midi.base(), Class { cell_bytes: 16, cells: 256 });
        assert_eq!(midi.largest_cell_bytes(), 65536, "a sysex dump");
        assert_eq!(midi.bytes_reserved(), 256 * 16 + 2 * 65536);
    }

    #[test]
    fn capacity_at_open_sizes_the_base_class_and_zero_keeps_the_profile() {
        let osc = default_for(KIND_OSC);
        assert_eq!(osc.with_base_depth(0), osc, "0 is the profile's own depth");
        let deeper = osc.with_base_depth(100);
        assert_eq!(deeper.base().cells, 128, "rounded up, as the header promises");
        assert_eq!(deeper.classes()[1..], osc.classes()[1..], "the wide classes are untouched");
    }

    #[test]
    fn an_installed_shape_replaces_the_default_for_its_kind_only() {
        const KIND: u32 = 77;   // a kind nothing else in the test binary installs
        assert_eq!(for_kind(KIND), default_for(KIND));
        assert!(install(KIND, vec![Class { cell_bytes: 32, cells: 4 }]));
        assert_eq!(for_kind(KIND), Layout::single(32, 4));
        assert!(!install(KIND, vec![]), "a bad shape is refused…");
        assert_eq!(for_kind(KIND), Layout::single(32, 4), "…and the good one stays");
        assert_eq!(for_kind(KIND + 1), default_for(KIND + 1), "another kind is untouched");
    }
}
