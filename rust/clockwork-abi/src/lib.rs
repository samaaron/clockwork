// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! clockwork's C ABI, as Rust.
//!
//! Six headers, six modules, declared by hand and CHECKED BY MACHINE:
//! `tests/layout.rs` compiles a C probe against the real headers and compares
//! the size of every struct and the offset of every field with what these
//! declarations say. A field added to a header without its twin here fails
//! that test rather than being read as another field's bytes on the next
//! boot — which is how a guest's hand-copied `DspConfig` once drifted three
//! fields out of line and read its config through the wrong pointers.
//!
//! * [`dsp_api`] — what a GUEST implements (`src/dsp_api.h`): the structs the
//!   host hands it and the entry points it exports.
//! * [`client`] — what a CLIENT calls (`src/clockwork_client.h`): a handle over
//!   the engine's memory, send and poll, regions, metrics, scopes.
//! * [`sink`] — where a guest's own events leave (`src/clockwork_event_sink.h`).
//! * [`lanes`] — what a HOST calls (`src/lanes/lanes.h`): boot, then per
//!   block write ingress, tick, read the block, drain egress. A plugin in a
//!   DAW's process callback is a host; so is the browser worklet.
//! * [`arena`] — the arena's table of contents (`src/clockwork_arena.h`):
//!   what any reader with the base pointer finds every region from.
//! * [`embed`] — an engine in your process (`src/clockwork_embed.h`): boot
//!   it on the device, or attach and render it yourself; a client either way.
//!
//! No types beyond `core::ffi`, no allocation, no dependencies: this crate is
//! the shape of the boundary and nothing else, so it links into a worklet, a
//! plugin or a device the same as into a desktop app. The functions are
//! declared `extern "C"` and resolved by whoever links the engine — the
//! umbrella staticlib natively, the module on the web. Enumerations that C
//! passes as `int` are newtypes here rather than Rust enums: a value the
//! header did not list must not be undefined behaviour to receive.
#![no_std]

pub mod dsp_api;
pub mod client;
pub mod sink;
pub mod lanes;
pub mod arena;
pub mod embed;
