// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! clockwork's timelines.
//!
//! A *timeline* is a beat grid: a tempo and one (beat, time) anchor, plus a
//! transport. Every beat<->time question clockwork answers — the `/clockwork/clock`
//! rpc verbs, a guest's `DspConfig::clock`, the plugin bridge's host
//! transport — is answered from a [`Timeline`] snapshot, in NTP seconds. Where
//! the snapshot came from (the Ableton Link session, a MIDI clock arriving on
//! a port, a local free-running grid) is the source's business and is not
//! visible in the value.
//!
//! * [`timeline`] — the snapshot and its arithmetic.
//! * [`midi`] — the registry of `midi:<port>` follower timelines: slot claim
//!   and LRU eviction, the pulse -> beat/tempo estimator,
//!   Start/Continue/Stop/SPP, staleness and primary selection.
//! * [`midi_clock_out`] — the other direction: which 24-PPQN pulses a MIDI
//!   clock *out* that follows a timeline owes, from the last one it sent.
//! * [`ffi`] — the `#[no_mangle] extern "C"` surface clockwork's C++ calls
//!   (`cpp/clockwork_clock.h`).
//!
//! Nothing here reads a clock. Every operation takes `now` from the caller,
//! which is what makes the whole crate testable without sleeping, and what
//! keeps it identical on native and web.
//!
//! Twins: the three-line beat arithmetic also exists in C++
//! (`src/clock/clock_math.h`, for the shared-memory mutators) and JS
//! (`js/lib/clock_math.js`, for the browser clock). They must agree.

pub mod ffi;
pub mod midi;
pub mod midi_clock_out;
pub mod timeline;

pub use midi::{is_timeline_name, Registry};
pub use timeline::Timeline;
