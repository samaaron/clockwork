// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The port substrate — frames crossing the audio-thread boundary.
//!
//! This crate is the implementation of `src/clockwork_ports.h`, and that header, not
//! this comment, is the contract. What is worth saying here is what the shape
//! of the code is and why it is in Rust:
//!
//! * [`ring`] — one SPSC lock-free frame ring. Interleaved storage, two
//!   monotonic counters, power-of-two capacity.
//! * [`registry`] — what sits in a slot: the ring, the channel count, the
//!   direction, the counters, and the arguments a port refuses. The slot
//!   discipline itself — the single atomic word per slot that makes a closed
//!   slot safe to read without a lock — is `clockwork-slots`, because
//!   `src/clockwork_event_sink.h` wants the same rule for a different payload and
//!   two copies of a lock-free lifetime discipline is one too many.
//! * [`ffi`] — the `#[no_mangle] extern "C"` surface clockwork calls.
//!
//! # What is deliberately absent
//!
//! Disk, network, Link, files, sockets, sample rates, formats. A port is a
//! slot, a direction, a channel count, a ring and two counters, and an
//! endpoint is whoever calls `clockwork_port_produce` or `clockwork_port_consume` from
//! some other thread. If a word for a particular kind of endpoint appears
//! anywhere in this crate, the generalisation has failed. The disk endpoints
//! live in `clockwork-ports-disk`, which depends on this crate; nothing here
//! depends on that one, and the dependency edge is what enforces the rule.
//!
//! # Why Rust
//!
//! Pure ring-and-slot logic with no C++ dependency, running
//! on the realtime thread, where a use-after-free is a crash in the audio
//! callback and memory safety is worth the most. Written in C++ first it would
//! only have been written twice.

pub mod ffi;
pub mod registry;
pub mod ring;

pub use registry::{DIR_SINK, DIR_SOURCE, MAX_PORTS};
