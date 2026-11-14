// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The parameter table as Rust (docs/PARAMS.md, `src/params/clockwork_params.h`).
//!
//! [`Reader`] parses the region's bytes directly, wherever they are mapped: an
//! engine in this process, another process's shared memory, a web snapshot, a
//! file. A panel or a visualiser calls [`Reader::refresh`] and [`Reader::value`]
//! each frame, and keeps no copy of its own.
//!
//! [`Param`] holds the one implementation of what a value means: its place on a
//! knob ([`Param::position_of`], [`Param::value_at`]) and its words
//! ([`Param::text_of`], [`Param::value_of_text`]). A bridge to a plugin format
//! answers its host from these and drops what the format cannot hold.
//!
//! [`Define`] is the other direction: entries and their text, as the define
//! verb and `DspHost::param_define` take them.

pub use clockwork_abi::params::*;

mod convert;
mod define;
mod reader;

pub use define::{Define, Say};
pub use reader::{Param, Reader};

#[cfg(target_arch = "wasm32")]
mod wasm;
