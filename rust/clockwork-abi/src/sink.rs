// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! `src/clockwork_event_sink.h` — where a guest's own events leave: a MIDI port
//! or an OSC destination, each message with its time, honoured at the edge.
//! A guest usually reaches these through `DspHost::open_sink` / `send_sink`
//! rather than by name; the names are here for a host, and for the stats.

use core::ffi::{c_char, c_int, c_void};

use crate::timetag::ClockworkTimetag;

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

/// The host's emitter (`clockwork_sink_set_host_emitter`): every MIDI sink
/// opened while one is installed hands each message to it with its time and
/// the emitter's context.
pub type ClockworkSinkHostEmit = Option<
    unsafe extern "C" fn(ctx: *mut c_void, kind: u32, target: *const c_char, bytes: *const u8, len: u32, when: ClockworkTimetag) -> c_int,
>;

/// The emitter and its context, installed together.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkSinkHostEmitter {
    pub emit: ClockworkSinkHostEmit,
    pub ctx: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ClockworkSinkStats {
    pub sent: u64,
    pub dropped: u64,
    pub late: u64,
    pub scheduled: u64,
    pub cancelled: u64,
}

/// A table of sinks (`ClockworkSinks`, clockwork_event_sink.h): a core's
/// (`clockwork_core_sinks`), or one a host made. Opaque.
#[repr(C)]
pub struct ClockworkSinks {
    _private: [u8; 0],
}

extern "C" {
    pub fn clockwork_sinks_new(core: *mut crate::lanes::ClockworkCore) -> *const ClockworkSinks;
    pub fn clockwork_sinks_free(sinks: *const ClockworkSinks);
    pub fn clockwork_sink_open(sinks: *const ClockworkSinks, kind: ClockworkSinkKind, target: *const c_char, capacity: u32) -> ClockworkSink;
    pub fn clockwork_sink_open_guest(sinks: *const ClockworkSinks, kind: ClockworkSinkKind, target: *const c_char, capacity: u32) -> ClockworkSink;
    pub fn clockwork_sink_close(sinks: *const ClockworkSinks, sink: ClockworkSink);
    pub fn clockwork_sink_close_guests(sinks: *const ClockworkSinks);
    pub fn clockwork_sink_set_host_emitter(sinks: *const ClockworkSinks, emitter: *const ClockworkSinkHostEmitter);
    pub fn clockwork_sink_profile(sinks: *const ClockworkSinks, kind: ClockworkSinkKind, n: u32, cell_bytes: *const u32, cells: *const u32) -> c_int;
    pub fn clockwork_sink_max_message_bytes(sinks: *const ClockworkSinks, sink: ClockworkSink) -> u32;
    pub fn clockwork_sink_bytes_reserved(sinks: *const ClockworkSinks, sink: ClockworkSink) -> u64;
    pub fn clockwork_sink_is_open(sinks: *const ClockworkSinks, sink: ClockworkSink) -> c_int;
    pub fn clockwork_sink_kind(sinks: *const ClockworkSinks, sink: ClockworkSink) -> ClockworkSinkKind;
    pub fn clockwork_sink_target(sinks: *const ClockworkSinks, sink: ClockworkSink) -> *const c_char;
    pub fn clockwork_sink_send(sinks: *const ClockworkSinks, sink: ClockworkSink, bytes: *const u8, len: u32, when: ClockworkTimetag) -> c_int;
    pub fn clockwork_sink_stats(sinks: *const ClockworkSinks, sink: ClockworkSink, out: *mut ClockworkSinkStats) -> c_int;
    pub fn clockwork_sink_flush(sinks: *const ClockworkSinks, sink: ClockworkSink) -> u32;
    pub fn clockwork_sink_flush_all(sinks: *const ClockworkSinks) -> u32;
    pub fn clockwork_sink_list(sinks: *const ClockworkSinks, out: *mut ClockworkSink, cap: u32) -> u32;
    pub fn clockwork_sink_close_all(sinks: *const ClockworkSinks);
}
