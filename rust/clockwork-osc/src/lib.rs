// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Shared, std-only helpers for clockwork's Rust subsystems (MIDI, gamepad, OSC):
//! a complete OSC 1.0 [`osc`] codec (messages + bundles), OSC-safe device-name
//! [`normalize`]ation,
//! and the native subsystems' common C-ABI scaffolding ([`ffi`]). Kept
//! dependency-free so it builds identically for the native staticlibs and the
//! wasm-bindgen modules.
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
pub mod osc;

pub use normalize::{assign_handle, normalize_ports, safe_osc_name, PortInfo};
pub use osc::{decode, decode_packet, encode, encode_packet, OscArg, OscBundle, OscMessage, OscPacket};
