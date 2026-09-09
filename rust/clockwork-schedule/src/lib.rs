// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Clockwork's timed-event store, and the wire forms that reach it.
//!
//! This crate is the implementation of `src/scheduler/Scheduler.h` and
//! `src/scheduler/schedule_parse.h`; those headers, not this comment, are the
//! contract, and `test/test_scheduler.cpp` and `test/test_schedule_parse.cpp`
//! are the specification. What is worth saying here is the shape:
//!
//! * [`tag`] — FNV-1a over a short string, clamped away from 0, plus the
//!   three tag constants clockwork reserves.
//! * [`store`] — the store itself: opaque payload bytes keyed by an int64
//!   timetag, released in time order, cancellable by tag. A fixed slot pool,
//!   a bump-allocated data pool with in-place compaction, and a binary
//!   min-heap of `(timetag, stability)`.
//! * [`parse`] — the two wire forms that carry a time: a timestamped OSC
//!   bundle, and the flat `"/clockwork/schedule <timetag> <blob>"`.
//! * [`ffi`] — the `#[no_mangle] extern "C"` surface clockwork's C++ calls.
//!
//! # The two halves are separately switchable
//!
//! `store` and `parse` are cargo features and both default on. A build whose
//! DSP holds its own schedule and whose host wants neither timed MIDI out nor
//! timed OSC forwarding takes `parse` alone: it has nothing to hold, but it
//! must still read a timestamped bundle's timetag, because that bundle is the
//! DSP's and forwarding it with its time flattened would be a lie. CMake's
//! `CLOCKWORK_SCHEDULER` is the switch, and `src/scheduler/engine_schedule.h`
//! states how it composes with the runtime `DspInfo::holds_schedule`.
//!
//! # What is deliberately absent
//!
//! What an event *means*. The store holds bytes and a caller-defined metadata
//! record it never reads; who delivers a due event, and to what, is entirely
//! the caller's business. There is no OSC dispatch here, no synth, no engine
//! and no notion of a block.
//!
//! # Real-time safety
//!
//! Every allocation this crate performs happens in [`store::Store::new`].
//! After that: no allocation, no locks, no system calls, no panics. The one
//! atomic is the cross-thread clear handshake, and it is a release/acquire
//! pair on a single `AtomicBool`. Compaction sorts with `sort_unstable_by_key`
//! into a scratch buffer sized at construction, so even the worst case on the
//! fire path touches no allocator.
//!
//! # Why Rust
//!
//! The store is platform-independent, it runs on
//! the realtime thread, and it hands out borrowed pointers into a pool it
//! compacts underneath them — precisely the shape where memory safety is
//! worth the most.

pub mod ffi;
#[cfg(feature = "parse")]
pub mod parse;
#[cfg(feature = "store")]
pub mod store;
#[cfg(feature = "store")]
pub mod tag;

#[cfg(feature = "parse")]
pub use parse::{Packet, SCHEDULE_ADDR};
#[cfg(feature = "store")]
pub use store::{Event, Store, MAX_SLOT_COUNT};
#[cfg(feature = "store")]
pub use tag::{tag_hash, TAG_DEFAULT, TAG_SYNTH};
