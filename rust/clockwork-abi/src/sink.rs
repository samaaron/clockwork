// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `src/clockwork_event_sink.h` — where a guest's own events leave: a MIDI port
//! or an OSC destination, each message with its time, honoured at the edge.
//! A guest usually reaches these through `DspHost::open_sink` / `send_sink`
//! rather than by name; the names are here for a host, and for the stats.

use core::ffi::{c_char, c_int};

/// A slot, stable for the life of the sink. 0 is never valid.
pub type ClockworkSink = u32;
pub const CLOCKWORK_SINK_NONE: ClockworkSink = 0;

/// `ClockworkSinkKind`. A newtype, as the client's enums are.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkSinkKind(pub c_int);

impl ClockworkSinkKind {
    pub const MIDI: ClockworkSinkKind = ClockworkSinkKind(1);
    pub const OSC: ClockworkSinkKind = ClockworkSinkKind(2);
}

/// The host's emitter (`clockwork_sink_set_host_emit`): every MIDI sink opened
/// while one is installed hands each message to it with its time.
pub type ClockworkSinkHostEmit =
    Option<unsafe extern "C" fn(kind: u32, target: *const c_char, bytes: *const u8, len: u32, when: i64) -> c_int>;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ClockworkSinkStats {
    pub sent: u64,
    pub dropped: u64,
    pub late: u64,
    pub scheduled: u64,
    pub cancelled: u64,
}

extern "C" {
    pub fn clockwork_sink_open(kind: ClockworkSinkKind, target: *const c_char, capacity: u32) -> ClockworkSink;
    pub fn clockwork_sink_close(sink: ClockworkSink);
    pub fn clockwork_sink_set_host_emit(emit: ClockworkSinkHostEmit);
    pub fn clockwork_sink_profile(kind: ClockworkSinkKind, n: u32, cell_bytes: *const u32, cells: *const u32) -> c_int;
    pub fn clockwork_sink_max_message_bytes(sink: ClockworkSink) -> u32;
    pub fn clockwork_sink_bytes_reserved(sink: ClockworkSink) -> u64;
    pub fn clockwork_sink_is_open(sink: ClockworkSink) -> c_int;
    pub fn clockwork_sink_kind(sink: ClockworkSink) -> ClockworkSinkKind;
    pub fn clockwork_sink_target(sink: ClockworkSink) -> *const c_char;
    pub fn clockwork_sink_send(sink: ClockworkSink, bytes: *const u8, len: u32, when: i64) -> c_int;
    pub fn clockwork_sink_stats(sink: ClockworkSink, out: *mut ClockworkSinkStats) -> c_int;
    pub fn clockwork_sink_flush(sink: ClockworkSink) -> u32;
    pub fn clockwork_sink_flush_all() -> u32;
    pub fn clockwork_sink_list(out: *mut ClockworkSink, cap: u32) -> u32;
    pub fn clockwork_sink_close_all();
}
