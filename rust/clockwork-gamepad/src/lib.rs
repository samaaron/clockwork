// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! clockwork game-controller subsystem.
//!
//! A gamepad subsystem within clockwork, structured like the MIDI one
//! (`clockwork-midi`). It compiles to two shapes from one codebase:
//!
//! * a **native rlib** folded into the `clockwork-native` staticlib linked
//!   into the C++ engine (gilrs: evdev / XInput; Apple's GameController
//!   framework on macOS), and
//! * a **wasm-bindgen module** loaded by the main-thread clockwork JS, where
//!   the web Gamepad API lives.
//!
//! The pure logic here — the `/clockwork/gamepad/*` [`schema`], the canonical
//! button/axis names and per-pad diffing in [`state`] — is shared across both
//! targets, so an event for the same physical input is identical on web and
//! native. Only the IO/FFI shells are platform-specific.

pub mod schema;
pub mod state;

pub use schema::{
    decode_out, encode_axis, encode_button, encode_devices, encode_devices_reply, OutCommand,
};
pub use state::{axis_name, button_name, PadEvent, PadState, AXES, BUTTONS};

// The OSC codec + name normalisation are shared with clockwork-midi.
pub use clockwork_osc::{normalize, osc};

// Device IO + FFI run on an OS poll thread (native): gilrs on Linux/Windows,
// Apple's GameController framework on macOS (Apple's drivers claim modern
// pads, so gilrs's IOKit backend never sees their input there). The web boundary
// exposes the same shared core to JS via wasm-bindgen; Gamepad API I/O is done
// in JS.
#[cfg(not(target_arch = "wasm32"))]
pub mod io;
#[cfg(all(not(target_arch = "wasm32"), not(target_os = "macos")))]
pub mod device;
#[cfg(target_os = "macos")]
pub mod gc;
#[cfg(not(target_arch = "wasm32"))]
pub mod ffi;
#[cfg(target_arch = "wasm32")]
pub mod wasm;

// The platform's GamepadIo backend, selected once for all consumers
// (ffi.rs, examples).
#[cfg(all(not(target_arch = "wasm32"), not(target_os = "macos")))]
pub use device as backend;
#[cfg(target_os = "macos")]
pub use gc as backend;
