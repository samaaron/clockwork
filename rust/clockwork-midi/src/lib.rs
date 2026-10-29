// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! clockwork MIDI subsystem.
//!
//! A midir-based MIDI subsystem within clockwork. It compiles to two shapes
//! from one codebase:
//!
//! * a **native rlib** folded into the `clockwork-native` staticlib linked
//!   into the C++ engine (CoreMIDI/ALSA/WinMM), and
//! * a **wasm-bindgen module** loaded by the main-thread clockwork JS, where Web
//!   MIDI lives.
//!
//! The pure logic here — message [`message`] parse/encode, device-name
//! [`normalize`]ation, and [`clock`] generation/estimation — is shared
//! across both targets. Only the IO/FFI shells are platform-specific.

pub mod clock;
pub mod message;
pub mod schema;
pub mod sync;

// The OSC codec + name normalisation live in the shared clockwork-osc crate
// (also used by clockwork-gamepad), exposed under this crate's module paths.
pub use clockwork_osc::{normalize, osc};

pub use clock::{ClockEstimator, EstimatorParams, PPQN};
pub use message::MidiMessage;
pub use normalize::{normalize_ports, safe_osc_name, PortInfo};
pub use osc::{OscArg, OscMessage};
pub use schema::{
    decode_out, encode_clock_bpm, encode_in, encode_ports, encode_ports_reply, OutCommand,
};
pub use sync::{transport_event, TransportEvent};

// Device IO + FFI use midir and OS threads (native). The web boundary exposes the
// same shared core to JS via wasm-bindgen; Web MIDI I/O is done in JS.
#[cfg(not(target_arch = "wasm32"))]
pub mod device;
#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;
#[cfg(not(target_arch = "wasm32"))]
pub mod stamp;
#[cfg(not(target_arch = "wasm32"))]
pub mod watcher;
#[cfg(target_arch = "wasm32")]
pub mod wasm;

/// The two byte seams of this crate, for the fuzzer and the corpus test: a
/// MIDI port's bytes ([`message::exercise_in`]) and a client's
/// `/clockwork/midi/*` command ([`schema::decode_out`]), each fed the same
/// input.
pub fn exercise(data: &[u8]) {
    message::exercise_in(data);
    if let Ok(OutCommand::Send { msg, .. }) = decode_out(data) {
        assert!(!msg.encode().is_empty(), "a decoded send encodes to nothing");
    }
}

#[cfg(test)]
mod corpus {
    #[test]
    fn every_corpus_file_is_exercised() {
        let n = clockwork_corpus::replay(env!("CARGO_MANIFEST_DIR"), super::exercise);
        assert!(n > 0, "no corpus under rust/clockwork-midi/corpus");
    }
}
