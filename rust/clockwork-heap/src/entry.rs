// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The thread's entry: which core this thread is inside, and what kind of
//! call put it there (docs/ENGINES.md).
//!
//! A core is entered by the call that works inside it — a block, a build, a
//! control pass — and left when that call returns, the caller's entry coming
//! back, so nesting gives back the caller's. A guest call or a door entered
//! inside one keeps its core and its hold on the core's gate, and says what
//! kind of call it is. Nothing remembers a core against a thread between
//! calls. The entry routes allocations and attributes log lines, and nothing
//! else reads it: everything else is handed its core.
//!
//! Here, in the lowest crate, because the global allocator reads it; the core
//! is opaque to it (`clockwork-core` owns the type), and so is the core's
//! heap, which the allocator reaches through it.

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr;

/// What kind of call a thread is in.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A call that works inside a core with no block rendering (an init, a
    /// rebuild, a teardown, a close), and a thread inside no core.
    Control = 0,
    /// A block.
    Render = 1,
    /// One of Clockwork's doors: a sink or a port opened, closed or
    /// reshaped. What it allocates is Clockwork's, whoever called it.
    Door = 2,
    /// A guest entry point at boot: `dsp_describe`, the memory queries,
    /// `dsp_new`, `dsp_free`.
    GuestBoot = 3,
    /// A guest entry point as it runs: `dsp_process`, `dsp_osc`, `dsp_asset`.
    GuestProcessing = 4,
}

impl Kind {
    /// The kind a C caller names, or `None` for a number that names none.
    pub fn from_u8(kind: u8) -> Option<Kind> {
        match kind {
            0 => Some(Kind::Control),
            1 => Some(Kind::Render),
            2 => Some(Kind::Door),
            3 => Some(Kind::GuestBoot),
            4 => Some(Kind::GuestProcessing),
            _ => None,
        }
    }

    /// Whether this is a guest entry point, at boot or as it runs.
    pub fn is_guest(self) -> bool {
        matches!(self, Kind::GuestBoot | Kind::GuestProcessing)
    }
}

/// A thread's entry: the core it is inside and that core's heap
/// (`core_heap::CoreHeap`), the kind of call, and whether the call holds the
/// core's gate (docs/ENGINES.md): the right to work on the
/// core's pools.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Entry {
    pub core: *mut c_void,
    pub heap: *const c_void,
    pub kind: Kind,
    pub gate: bool,
}

impl Entry {
    /// A thread inside no core.
    pub const NONE: Entry = Entry { core: ptr::null_mut(), heap: ptr::null(), kind: Kind::Control, gate: false };
}

thread_local! {
    static ENTRY: Cell<Entry> = const { Cell::new(Entry::NONE) };
}

/// Make `entry` this thread's. Returns the entry the thread had, to give
/// back to [`leave`].
pub fn enter(entry: Entry) -> Entry {
    ENTRY.with(|e| e.replace(entry))
}

/// A call of `kind` inside the one this thread is in: the same core, the
/// same hold on its gate. Returns the entry the thread had, for [`leave`].
pub fn within(kind: Kind) -> Entry {
    ENTRY.with(|e| {
        let here = e.get();
        e.replace(Entry { kind, ..here })
    })
}

/// Leave the entry made last, giving back the one the thread had before it.
pub fn leave(previous: Entry) {
    ENTRY.with(|e| e.set(previous));
}

/// This thread's entry: [`Entry::NONE`] when it is inside no core.
pub fn current() -> Entry {
    ENTRY.with(Cell::get)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn at(core: usize, kind: Kind, gate: bool) -> Entry {
        Entry { core: core as *mut c_void, heap: ptr::null(), kind, gate }
    }

    #[test]
    fn nesting_gives_back_the_callers_entry() {
        assert_eq!(current(), Entry::NONE);
        let outer = enter(at(8, Kind::Render, true));
        assert_eq!(outer, Entry::NONE);
        assert_eq!(current(), at(8, Kind::Render, true));
        let inner = enter(at(16, Kind::Control, false));
        assert_eq!(inner, at(8, Kind::Render, true));
        assert_eq!(current(), at(16, Kind::Control, false));
        leave(inner);
        assert_eq!(current(), at(8, Kind::Render, true));
        leave(outer);
        assert_eq!(current(), Entry::NONE);
    }

    #[test]
    fn a_door_inside_a_guest_call_keeps_the_core_and_the_gate_and_gives_the_guest_call_back() {
        let block = enter(at(8, Kind::Render, true));
        let guest = within(Kind::GuestProcessing);
        assert_eq!(current(), at(8, Kind::GuestProcessing, true));
        assert!(current().kind.is_guest());
        let door = within(Kind::Door);
        assert_eq!(current(), at(8, Kind::Door, true));
        assert!(!current().kind.is_guest());
        leave(door);
        assert_eq!(current(), at(8, Kind::GuestProcessing, true));
        leave(guest);
        assert_eq!(current(), at(8, Kind::Render, true));
        leave(block);
    }

    #[test]
    fn a_door_outside_any_core_holds_no_gate() {
        let door = within(Kind::Door);
        assert_eq!(current(), Entry { kind: Kind::Door, ..Entry::NONE });
        leave(door);
        assert_eq!(current(), Entry::NONE);
    }

    #[test]
    fn each_thread_has_its_own() {
        let previous = enter(at(8, Kind::GuestBoot, true));
        std::thread::spawn(|| assert_eq!(current(), Entry::NONE)).join().unwrap();
        assert_eq!(current(), at(8, Kind::GuestBoot, true));
        leave(previous);
    }

    #[test]
    fn a_kind_from_c_is_one_of_the_five_or_none() {
        for k in [Kind::Control, Kind::Render, Kind::Door, Kind::GuestBoot, Kind::GuestProcessing] {
            assert_eq!(Kind::from_u8(k as u8), Some(k));
        }
        assert_eq!(Kind::from_u8(5), None);
        assert_eq!(Kind::from_u8(255), None);
    }
}
