// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Shared helpers for clockwork's Rust subsystems (MIDI, gamepad, OSC) and for
//! guests: the OSC 1.0 codec ([`wire`]: messages and bundles, read and written
//! in place with no allocation), the [`Timetag`], OSC-safe device-name
//! [`normalize`]ation, and the native subsystems' common C-ABI scaffolding
//! ([`ffi`]). Kept dependency-free so it builds identically for the native
//! staticlibs and the wasm-bindgen modules.
#![forbid(unsafe_code)]

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

/// The one address prefix clockwork reserves for itself.
///
/// The split between clockwork and the DSP is mechanical: an address
/// beginning `/clockwork/` is the Clockwork's, and ANYTHING else is forwarded to
/// the DSP untouched. These crates sit on clockwork's side, so every address
/// they encode or match must be built with this macro rather than written out.
///
/// `clockwork_sys!()` is the prefix; `"/clockwork/midi/ports"` is
/// `"/clockwork/midi/ports"`, concatenated at compile time — so it is usable in a
/// `match` pattern, not only in an expression.
///
/// THIS IS THE ONLY PLACE THE STRING IS SPELLED in the Rust tree (the C++ tree
/// has its own single definition, `CLOCKWORK_SYS_PREFIX_LIT` in `src/clockwork_prefix.h`;
/// the two must agree). Changing the prefix is a change to the first arm below
/// and nothing else.
#[macro_export]
macro_rules! clockwork_sys {
    ()           => { "/clockwork/" };
    ($suffix:literal) => { concat!($crate::clockwork_sys!(), $suffix) };
}

/// The reserved prefix as a value, for the rare caller that needs it at runtime.
pub const CLOCKWORK_SYS_PREFIX: &str = clockwork_sys!();

#[allow(unused_imports)]
use alloc::{borrow::ToOwned, boxed::Box, format, string::{String, ToString}, vec, vec::Vec};
#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;
pub mod normalize;
pub mod timetag;
pub mod wire;

pub use normalize::{assign_handle, normalize_ports, safe_osc_name, PortInfo};
pub use timetag::Timetag;
#[cfg(feature = "std")]
pub use timetag::{delay_until, now_timetag, timetag_of};

/// Why a subsystem refused one of its verbs. The engine answers it with
/// `/clockwork/error <address> <reason> <generation>`, in the words the
/// web's host front uses (`js/lib/host_front.js`) and with the guest
/// instance's generation at that moment (`docs/RESTARTS.md`), so a client
/// reads the same refusal on every host.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refusal {
    /// Under the subsystem's namespace, but not one of its verbs: a typo, or
    /// a verb from a newer client.
    Unknown,
    /// One of its verbs, with arguments it cannot read.
    Malformed,
}

impl Refusal {
    /// The reason, NUL-terminated so a C caller can send it as it is.
    pub const fn reason(self) -> &'static core::ffi::CStr {
        match self {
            Refusal::Unknown => c"unknown clockwork verb",
            Refusal::Malformed => c"malformed",
        }
    }
}

/// What the fuzzer does to bytes from the network, as a function: read them
/// as a message and as a packet, and check that what reads is written back in
/// no more room than it came in, and that what was written reads and writes
/// to itself. rust/fuzz's `osc_packet` target calls this under libFuzzer; the
/// corpus test calls it over corpus/ on every push.
pub fn exercise(data: &[u8]) {
    let alone = wire::Message::parse(data);
    let Ok(first) = wire::Packet::parse(data) else {
        assert!(alone.is_err(), "a message that is no packet");
        return;
    };
    assert_eq!(alone.is_ok(), matches!(first, wire::Packet::Message(_)), "a message is a packet of one");
    let mut once = vec![0u8; data.len()];
    let once = first.write(&mut once).expect("what was read is written where it came from");
    let again = wire::Packet::parse(once).expect("a written packet reads");
    let mut twice = vec![0u8; once.len()];
    assert_eq!(again.write(&mut twice), Ok(once), "writing is not stable");
}

#[cfg(test)]
mod corpus {
    #[test]
    fn every_corpus_file_is_exercised() {
        let n = clockwork_corpus::replay(env!("CARGO_MANIFEST_DIR"), super::exercise);
        assert!(n > 0, "no corpus under rust/clockwork-osc/corpus");
    }
}
