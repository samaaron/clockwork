// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! Umbrella staticlib for the clockwork native Rust subsystems.
//!
//! A binary may link exactly one Rust staticlib (each staticlib bundles its own
//! copy of std, so two collide at final link). This crate is that staticlib:
//! the subsystems are plain rlib dependencies, and the `pub use` below keeps
//! their `#[no_mangle]` C ABI symbols (`clockwork_midi_*`, `clockwork_gamepad_*`, `clockwork_osc_*`)
//! anchored in the archive CMake links into the host. The C headers live with
//! the subsystems: `clockwork-midi/cpp/clockwork_midi.h`,
//! `clockwork-gamepad/cpp/clockwork_gamepad.h`, `clockwork-osc-net/cpp/clockwork_osc.h`.
#![forbid(unsafe_code)]

/// The growable pool every allocation that matters comes from. Not optional:
/// whatever guest is attached allocates from it, so it is in the archive
/// whatever else this build turned off.
pub use clockwork_heap;

/// Scope slot claim and release, for the same reason: clockwork's, and used
/// by whichever guest is attached.
pub use clockwork_scope;

/// The beat grid as a value (`clockwork_timeline_*`) and the MIDI-clock follower
/// registry (`clockwork_midi_timelines_*`, src/clock/MidiTimelines.h wraps it).
pub use clockwork_clock;

/// The port substrate: frames crossing the audio-thread boundary on a stable
/// slot (`clockwork_port_*`, src/clockwork_ports.h). Not optional — `audio_processor.cpp`
/// calls into it every block.
pub use clockwork_ports;

/// The event-sink substrate: messages leaving clockwork on a stable slot,
/// in time order (`clockwork_sink_*`, src/clockwork_event_sink.h). Not optional —
/// `DspHost` hands every DSP a sink handle and a send. Its endpoints are: a
/// build without the `midi` or `osc` feature carries no MIDI or OSC endpoint
/// and refuses to open a sink of that kind.
pub use clockwork_sinks;

/// The disk endpoints built on it (`clockwork_disk_*`,
/// clockwork-ports-disk/cpp/clockwork_disk.h). Separate from the substrate on
/// purpose: the dependency runs disk -> ports and cargo would refuse the
/// reverse, so "the substrate mentions no endpoint" is checked by the build.
pub use clockwork_ports_disk;

// No DSP and no guest are re-exported here, because neither is in this
// repository. The guest supplies `clockwork_engine_*` / `clockwork_perform_osc_*` itself
// (see src/engine_api.h and src/scheduler/guest_schedule.h) and is linked
// alongside this archive, not from inside it.

#[cfg(feature = "gamepad")]
pub use clockwork_gamepad;
/// The wire forms that carry a time, and — under the `schedule` feature — the
/// timed-event store behind them (`clockwork_sched_*`,
/// clockwork-schedule/cpp/clockwork_schedule.h).
///
/// The STORE is optional and the option is the point: a build whose DSP holds
/// its own schedule and whose host wants neither timed MIDI out nor timed OSC
/// forwarding carries no slot pool and no data pool at all. CMake's
/// CLOCKWORK_SCHEDULER drives it and gates the C++ to match. The wire forms
/// are not optional, because a timestamped bundle belongs to the DSP and a
/// build with no store must still read its timetag to hand it over honestly.
pub use clockwork_schedule;
#[cfg(feature = "midi")]
pub use clockwork_midi;
#[cfg(feature = "osc")]
pub use clockwork_osc_net;
#[cfg(feature = "comms")]
pub use clockwork_comms;
