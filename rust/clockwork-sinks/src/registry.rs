// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The sink table: which endpoint a kind means, and who drains it.
//!
//! The slot discipline is not here — it is `clockwork-slots`, shared with
//! `clockwork-ports`. `src/clockwork_event_sink.h` and `src/clockwork_ports.h` each ask
//! for the same thing in their own words: a handle stable for the life of what
//! it names, a close that bites the instant it is called while the audio
//! thread may be inside, and no lock on the hot path. One implementation,
//! one set of arguments, one set of tests.
//!
//! What is here is the two decisions that are a SINK's:
//!
//! * **A kind chooses an endpoint, and a kind this build cannot reach is
//!   refused.** `clockwork_sink_open` names MIDI or OSC, so unlike ports this
//!   substrate cannot be endpoint-blind. A build compiled without the MIDI
//!   subsystem answers `CLOCKWORK_SINK_NONE` for a MIDI sink rather than opening one
//!   that silently swallows everything.
//! * **Who pumps.** Almost always the crate's own drain thread, because the
//!   header exposes no drain call — a sink is opened and it works. A caller
//!   that wants to drive the drain itself (a test asserting on order at an
//!   exact instant; a host that would rather own the thread) says so, and the
//!   service leaves that sink alone.

use std::ffi::CString;
use std::sync::Arc;

use clockwork_slots::SlotTable;

use crate::endpoint::Endpoint;
use crate::profile::{self, Layout};
use crate::sink::{Sink, KIND_MIDI, KIND_OSC};

/// Slots in the table. Matches `clockwork-ports`'s ceiling, for no better
/// reason than that neither has ever wanted more and a reader should not have
/// to remember two numbers.
pub const MAX_SINKS: usize = 64;

/// Who calls the drain.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Drive {
    /// The crate's own thread, started on the first such sink. What every sink
    /// opened through the C ABI gets: `src/clockwork_event_sink.h` has no drain
    /// call, so the substrate owns the thread.
    Service,
    /// Nobody, until the caller calls [`pump`]. For a test that needs to say
    /// exactly when "now" is, and for a host that would rather own the thread.
    Manual,
    /// Nobody, ever: a send IS the delivery. For an endpoint that is safe to
    /// call on the audio thread and holds the message to its time itself —
    /// the host's, on a target with no thread to pump from
    /// ([`crate::endpoint::HostEndpoint`]). Nothing is queued, so nothing
    /// can be pumped or flushed.
    Direct,
}

static SINKS: SlotTable<Sink, MAX_SINKS> = SlotTable::new();

/// The table itself, for the slot diagnostics.
pub fn table() -> &'static SlotTable<Sink, MAX_SINKS> { &SINKS }

/// Take a reference on a sink, or `None` if the handle is stale, closed, or
/// was never valid. Allocation-free and lock-free: this is what
/// `clockwork_sink_send` goes through on the audio thread.
#[inline]
pub fn acquire(handle: u32) -> Option<clockwork_slots::Held<'static, Sink>> {
    SINKS.acquire(handle)
}

/// Build the endpoint a kind and target name, or `None` if this build cannot
/// reach it or the target does not exist.
fn endpoint_for(kind: u32, target: &str) -> Option<Arc<dyn Endpoint>> {
    match kind {
        #[cfg(all(feature = "midi", not(target_arch = "wasm32")))]
        KIND_MIDI => Some(Arc::new(crate::endpoint::MidiEndpoint::open(target)?)),
        #[cfg(all(feature = "osc", not(target_arch = "wasm32")))]
        KIND_OSC => Some(Arc::new(crate::endpoint::OscEndpoint::open(target)?)),
        // A kind this build has no endpoint for. Refusing beats opening
        // something that accepts every message and delivers none.
        _ => { let _ = target; None }
    }
}

/// Open a sink onto a real destination, shaped by the kind's profile with
/// its base class `capacity` deep (0 = the profile's own depth). Control
/// thread: this allocates and may touch the device.
pub fn open(kind: u32, target: &str, capacity: u32) -> u32 {
    // The host first, when it has said it takes MIDI (endpoint::set_host_emit):
    // on a target with no port and no thread — the worklet — that is the only
    // way out, and driven DIRECT, because the emitter is a ring write and
    // there is nobody to pump.
    if kind == KIND_MIDI {
        if let Some(host) = crate::endpoint::HostEndpoint::for_target(kind, target) {
            return open_with(kind, target, capacity, Arc::new(host), Drive::Direct);
        }
    }
    let Some(endpoint) = endpoint_for(kind, target) else { return 0 };
    open_with(kind, target, capacity, endpoint, Drive::Service)
}

/// Open a sink onto an endpoint the caller supplies.
///
/// The boundary the tests inject through, and the one place a harness could add a
/// third kind of destination without the substrate learning what it is.
pub fn open_with(kind: u32, target: &str, capacity: u32,
                 endpoint: Arc<dyn Endpoint>, drive: Drive) -> u32 {
    let layout = profile::for_kind(kind).with_base_depth(capacity);
    open_with_layout(kind, target, &layout, endpoint, drive)
}

/// Open a sink of an exact shape, profile or no profile.
pub fn open_with_layout(kind: u32, target: &str, layout: &Layout,
                        endpoint: Arc<dyn Endpoint>, drive: Drive) -> u32 {
    if kind != KIND_MIDI && kind != KIND_OSC { return 0; }
    let cname = match CString::new(target) {
        Ok(c) => c,
        // An interior NUL: the target is a C string and cannot carry one.
        Err(_) => return 0,
    };
    let layout = layout.clone();
    let handle = SINKS.open(move || Sink::new(kind, cname, &layout, endpoint, drive));
    if handle != 0 && drive == Drive::Service {
        crate::service::ensure_running();
    }
    handle
}

/// Close a sink. Silent to new sends immediately; freed as soon as nobody is
/// inside. Idempotent and safe against a stale handle.
pub fn close(handle: u32) {
    SINKS.close(handle);
}

/// Every open sink's handle, in slot order.
///
/// # Safety
/// `out` is valid for `cap` writes, or null when `cap` is 0.
pub unsafe fn list(out: *mut u32, cap: u32) -> u32 {
    // SAFETY: the contract above is `list_raw`'s.
    unsafe { SINKS.list_raw(out, cap) }
}

/// How many sinks are open. What the drain thread uses to decide it has
/// nothing left to do.
pub fn open_count() -> u32 {
    // SAFETY: null with a zero cap is the documented "count only" call.
    unsafe { SINKS.list_raw(core::ptr::null_mut(), 0) }
}

/// Close every sink. For shutdown, and so one test case cannot leak slots into
/// the next.
/// Cancel everything the sink is holding. 0 if the handle is not live.
pub fn flush(handle: u32) -> u32 {
    acquire(handle).map(|s| s.flush()).unwrap_or(0)
}

/// Cancel everything EVERY open sink is holding, and say how many went.
///
/// This is what a run-stop reaches for: one call, every port and every
/// destination, whoever queued them. A guest that sent its own events with
/// clockwork_sink_send is covered by the same call as a client that sent verbs —
/// which is the point of putting cancellation on the sink rather than on
/// whichever queue a message happened to pass through.
pub fn flush_all() -> u32 {
    // The table's own enumeration, the same one clockwork_sink_list uses — a
    // fixed-size buffer rather than an allocation, so this is callable from
    // anywhere a stop button can be pressed.
    let mut handles = [0u32; MAX_SINKS];
    let n_open = table().list(&mut handles) as usize;
    let mut n = 0;
    for &handle in handles.iter().take(n_open.min(MAX_SINKS)) {
        n += flush(handle);
    }
    n
}

pub fn close_all() {
    SINKS.close_all();
}

/// Drain one sink at `now`. Returns the earliest timetag still held.
pub fn pump(handle: u32, now: u64) -> Option<u64> {
    acquire(handle)?.pump(now)
}

/// Drain every sink the service is responsible for. Returns the earliest
/// timetag held by any of them.
pub fn pump_service(now: u64) -> Option<u64> {
    let mut handles = [0u32; MAX_SINKS];
    let n = SINKS.list(&mut handles).min(MAX_SINKS as u32) as usize;
    let mut soonest: Option<u64> = None;
    for &h in &handles[..n] {
        let Some(sink) = acquire(h) else { continue };   // closed under us
        if sink.drive != Drive::Service { continue; }
        if let Some(w) = sink.pump(now) {
            soonest = Some(soonest.map_or(w, |s: u64| s.min(w)));
        }
    }
    soonest
}
