// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Umbrella staticlib for the plugin bridge (`src/native/PluginBridgeMain.cpp`).
//!
//! A binary may link exactly one Rust staticlib, and the bridge is a separate
//! binary from the engine: it runs the plugins that the engine will not have
//! in its own process, and meets the engine in a shared-memory segment whose
//! audio rings are ports over shared storage (`clockwork_port_open_shared`), and
//! the timelines it renders against arrive as snapshots whose beat and bar
//! arithmetic is clockwork-clock's (`clockwork_timeline_*`). That is the whole of what it
//! needs from Rust, so that is the whole of what this archive carries: no
//! registry, no MIDI, no OSC.
#![forbid(unsafe_code)]

/// The port substrate (`clockwork_port_*`, src/clockwork_ports.h), shared-storage rings
/// included.
pub use clockwork_ports;
/// The timeline snapshot's arithmetic (`clockwork_timeline_*`, clockwork_clock.h).
pub use clockwork_clock;
