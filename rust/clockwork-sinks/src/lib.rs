// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The event-sink substrate — messages leaving clockwork.
//!
//! This crate is the implementation of `src/clockwork_event_sink.h`, and that
//! header, not this comment, is the contract. What is worth saying here is the
//! shape of the code and the two decisions the header could not make because
//! nothing existed yet when it was written:
//!
//! * [`queue`] — one bounded, allocation-free message queue. Multi-producer
//!   (the header's own diagram has two: a DSP on the audio thread and
//!   clockwork's own scheduler on a control thread), single-consumer, and full
//!   means DROP rather than wait.
//! * [`profile`] — the size classes a sink of each kind is made of: several
//!   queues of different cell widths, so a three-byte note and a sysex dump
//!   each take a cell their own size.
//! * [`sink`] — a slot's occupant: that queue, the counters, and the drain
//!   that puts messages back into time order before they reach an endpoint.
//! * [`endpoint`] — where a message actually goes. A MIDI port, an OSC
//!   destination, or a capture buffer for a test.
//! * [`service`] — the one thread that drains every sink. Off the audio
//!   thread, which is the whole point.
//! * [`ffi`] — the `#[no_mangle] extern "C"` surface clockwork calls.
//!
//! # Why this is not `clockwork-ports`
//!
//! `clockwork_ports.h` says it outright: frames and events "are deliberately not one
//! type, because frames want a fixed cadence and silence-on-underrun while
//! events want variable size and a counted drop". Everything below that
//! sentence is different — a ring of interleaved floats against a queue of
//! sized blobs, a shortfall zeroed against a message dropped, a cadence
//! against a schedule.
//!
//! There is also a rule in that crate this one could not have kept: "if a word
//! for a particular kind of endpoint appears anywhere in this crate, the
//! generalisation has failed". A sink cannot obey it. `clockwork_sink_open` takes a
//! `ClockworkSinkKind`, so MIDI and OSC are in the ABI this crate exists to
//! implement, and a substrate that mentioned no endpoint could not implement
//! its own header. Ports could push its endpoints into `clockwork-ports-disk`
//! because `clockwork_port_open` never asks what an endpoint is; sinks cannot,
//! because `clockwork_sink_open` does.
//!
//! What the two genuinely share is the slot: a stable handle, a close that
//! bites at once while the audio thread is inside, and no lock on the hot
//! path. That is `clockwork-slots`, and it is shared as code rather than as a
//! second implementation of the same argument.
//!
//! # Time is a `u64`, whatever the header's signature says
//!
//! `clockwork_sink_send` takes `int64_t when`, matching `dsp_process`'s
//! `block_time`. An OSC timetag is 32.32 fixed point NTP seconds since 1900,
//! and 2026 is past 2^31 seconds from 1900 — so the top bit of a REAL timetag
//! is already set, and every current timetag is NEGATIVE as an `int64_t`.
//! Comparing two of them signed would put next Tuesday before "immediately".
//! So every comparison here converts to `u64` first, and 1 (the OSC value for
//! "immediately") is then the smallest thing there is, which is exactly right.

pub mod endpoint;
pub mod ffi;
pub mod profile;
pub mod queue;
pub mod registry;
pub mod service;
pub mod sink;
pub mod time;

pub use endpoint::{CaptureEndpoint, Delivery, Endpoint, HostEmit, HostEndpoint};
pub use registry::{Drive, MAX_SINKS};
pub use sink::{Stats, KIND_MIDI, KIND_OSC};
