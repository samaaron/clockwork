// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! clockwork's client boundary, safe.
//!
//! `clockwork_client.h` is the contract between the engine and anything that
//! drives it from outside the audio thread; `clockwork_embed.h` is the door
//! that puts an engine in your process. [`clockwork_abi`] declares both as
//! Rust and holds them to the headers by a compiled probe. This crate is the
//! layer above: the same calls with the pointer arithmetic, the lifetimes and
//! the close-on-drop written once, so a host writes its playing against
//! slices and `Result`s and carries no `unsafe` of its own.
//!
//! * [`Client`] — a handle over an engine's memory, however it was reached:
//!   another process's segment ([`Client::open_shm`]), this address space,
//!   or the engine an [`Embed`] owns. Send OSC bytes in ([`Client::send`],
//!   or build them in the ring with [`Client::send_with`]); take replies out
//!   ([`Client::poll`], [`Client::drain`], [`Client::poll_until`]); watch a
//!   ring without taking ([`Tap`]); reach a [`Region`]; read the counters as
//!   a block ([`Metrics`], [`NativeStats`]) and ask for them by name
//!   ([`Metric`], [`NativeStat`], generated from the schema); take one
//!   coherent [`Clock`] snapshot; read a [`Scope`] slot.
//! * [`Embed`] — an engine in this process: [`Embed::boot`] opens the audio
//!   device and ticks itself; [`Embed::attach`] opens nothing and you render
//!   ([`Embed::render`], or a [`Renderer`] on the audio thread while the
//!   client half stays with the UI). Either way the engine's client is
//!   [`Embed::client`], and closing the handle closes it.
//!
//! The symbols are resolved by whoever links the engine. A Rust host names
//! `clockwork-sys` (which builds and links the C++) and `clockwork-native`
//! with its `host` feature (the subsystems the engine calls back into), as
//! this crate's own tests do. Nothing here is real-time safe except
//! rendering, which is the audio thread's by design; every other call is a
//! client thread's. One handle is one thread's at a time — the wrappers are
//! [`Send`] and not [`Sync`] for that reason.

// The tests are a Rust host: the engine (clockwork-sys) and the subsystems
// (clockwork-native) must be linked, and a crate nothing names is not.
#[cfg(test)]
extern crate clockwork_native;
#[cfg(test)]
extern crate clockwork_sys;

mod client;
mod embed;
mod error;
mod metrics;
pub mod metrics_schema;
mod scope;
pub mod time;

pub use clockwork_abi as abi;

pub use client::{
    Client, Clock, Features, Info, Message, Messages, Region, RegionId, Ring, Route, Tap,
    POLL_BATCH,
};
pub use embed::{Device, Embed, EmbedConfig, Renderer, RENDER_MAX_CHANNELS};
pub use error::{Error, Result};
pub use metrics::{Metrics, NativeStats, METRICS_WORDS_MAX, NATIVE_STATS_WORDS_MAX};
pub use metrics_schema::{Metric, NativeStat};
pub use scope::{Scope, ScopeRead, SCOPE_MAX_CHANNELS};
