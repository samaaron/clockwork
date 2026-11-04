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
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::{Arc, Mutex};

use clockwork_log::{ClockworkCore, CoreRef};
use clockwork_slots::SlotTable;

use crate::endpoint::{Endpoint, HostEmitter, HostEndpoint};
use crate::profile::{self, Class, Layout};
use crate::sink::{Sink, KIND_MIDI, KIND_OSC};
use clockwork_osc::Timetag;

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
    /// Nobody, until the caller calls [`Sinks::pump`]. For a test that needs to say
    /// exactly when "now" is, and for a host that would rather own the thread.
    Manual,
    /// Nobody, ever: a send IS the delivery. For an endpoint that is safe to
    /// call on the audio thread and holds the message to its time itself —
    /// the host's, on a target with no thread to pump from
    /// ([`crate::endpoint::HostEndpoint`]). Nothing is queued, so nothing
    /// can be pumped or flushed.
    Direct,
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

/// A table of sinks: a core's (clockwork-core), or a host's own — a test's.
/// A sink's handle names a slot in the table it was opened in and in no
/// other. Each table also holds what its sinks are made by: the shapes
/// installed for each kind, and the host's emitter while the host is the
/// far end. Made only counted ([`Sinks::new`]), so the one drain thread can
/// walk every live table.
pub struct Sinks {
    /// The core whose table this is: what its lines name. Null for a host's
    /// own table.
    core: CoreRef,
    table: SlotTable<Sink, MAX_SINKS>,
    /// Kind → the layout installed for it. Consulted at OPEN only, a control
    /// thread's call, so a lock here costs the audio thread nothing.
    shapes: Mutex<Vec<(u32, Layout)>>,
    /// The host's emitter while it takes MIDI (`clockwork_sink_set_host_emitter`):
    /// a record the installer keeps alive until it installs another or none.
    host_emitter: AtomicPtr<HostEmitter>,
}

// What a table still holds when it goes is closed with it, so no sink — nor
// the endpoint inside one, which may point back at this table's emitter —
// outlives it.
impl Drop for Sinks {
    fn drop(&mut self) {
        self.table.close_all();
        crate::service::DRAIN.ring();
    }
}

impl Sinks {
    /// A new table, `core`'s, known to the drain from now on.
    pub fn new(core: *mut ClockworkCore) -> Arc<Sinks> {
        let t = Arc::new(Sinks {
            core: CoreRef::new(core),
            table: SlotTable::new(),
            shapes: Mutex::new(Vec::new()),
            host_emitter: AtomicPtr::new(core::ptr::null_mut()),
        });
        crate::service::register(&t);
        t
    }

    /// The table itself, for the slot diagnostics.
    pub fn table(&self) -> &SlotTable<Sink, MAX_SINKS> { &self.table }

    /// Take a reference on a sink, or `None` if the handle is stale, closed,
    /// or was never valid. Allocation-free and lock-free: this is what
    /// `clockwork_sink_send` goes through on the audio thread.
    #[inline]
    pub fn acquire(&self, handle: u32) -> Option<clockwork_slots::Held<'_, Sink>> {
        self.table.acquire(handle)
    }

    // ── Shapes ──────────────────────────────────────────────────────────

    /// Install a shape for a kind. Returns false for a shape that does not
    /// normalise; the previous shape (or the default) then stays.
    pub fn install_shape(&self, kind: u32, classes: Vec<Class>) -> bool {
        let Some(layout) = Layout::normalise(classes) else { return false };
        let Ok(mut shapes) = self.shapes.lock() else { return false };
        shapes.retain(|(k, _)| *k != kind);
        shapes.push((kind, layout));
        true
    }

    /// The shape a sink of `kind` is built to: installed, else the default.
    pub fn shape_for(&self, kind: u32) -> Layout {
        if let Ok(shapes) = self.shapes.lock() {
            if let Some((_, l)) = shapes.iter().find(|(k, _)| *k == kind) {
                return l.clone();
            }
        }
        profile::default_for(kind)
    }

    // ── The host as the endpoint ────────────────────────────────────────

    /// Install (or, with null, remove) the host's emitter. While one is
    /// installed, every MIDI sink opened in this table goes to it.
    pub fn set_host_emitter(&self, emitter: *const HostEmitter) {
        self.host_emitter.store(emitter.cast_mut(), Ordering::Release);
    }

    /// Whether the host has said it takes MIDI.
    pub fn host_emits(&self) -> bool {
        !self.host_emitter.load(Ordering::Acquire).is_null()
    }

    // ── Opening and closing ─────────────────────────────────────────────

    /// Open a sink onto a real destination, shaped by the kind's profile with
    /// its base class `capacity` deep (0 = the profile's own depth). `guest`
    /// says the guest opened it: closed with its instance
    /// ([`Sinks::close_guests`]). Control thread: this allocates and may touch
    /// the device.
    pub fn open(&self, kind: u32, target: &str, capacity: u32, guest: bool) -> u32 {
        // The host first, when it has said it takes MIDI: on a target with no
        // port and no thread — the worklet — that is the only way out, and
        // driven DIRECT, because the emitter is a ring write and there is
        // nobody to pump.
        if kind == KIND_MIDI && self.host_emits() {
            if let Some(host) = HostEndpoint::for_target(kind, target, &self.host_emitter) {
                return self.open_with(kind, target, capacity, Arc::new(host), Drive::Direct, guest);
            }
        }
        let Some(endpoint) = endpoint_for(kind, target) else { return 0 };
        self.open_with(kind, target, capacity, endpoint, Drive::Service, guest)
    }

    /// Open a sink onto an endpoint the caller supplies.
    ///
    /// The boundary the tests inject through, and the one place a harness
    /// could add a third kind of destination without the substrate learning
    /// what it is.
    pub fn open_with(&self, kind: u32, target: &str, capacity: u32,
                     endpoint: Arc<dyn Endpoint>, drive: Drive, guest: bool) -> u32 {
        let layout = self.shape_for(kind).with_base_depth(capacity);
        self.open_with_layout(kind, target, &layout, endpoint, drive, guest)
    }

    /// Open a sink of an exact shape, profile or no profile.
    pub fn open_with_layout(&self, kind: u32, target: &str, layout: &Layout,
                            endpoint: Arc<dyn Endpoint>, drive: Drive, guest: bool) -> u32 {
        if kind != KIND_MIDI && kind != KIND_OSC { return 0; }
        let cname = match CString::new(target) {
            Ok(c) => c,
            // An interior NUL: the target is a C string and cannot carry one.
            Err(_) => return 0,
        };
        let layout = layout.clone();
        let handle = self.table.open(move || Sink::new(kind, cname, &layout, endpoint, drive, guest));
        if handle != 0 && drive == Drive::Service {
            crate::service::ensure_running(self.core);
        }
        handle
    }

    /// Close a sink. New sends are refused at once, and the call returns once
    /// no send or delivery is still using the sink. Rings the drain so it
    /// stops when the last sink has gone. Idempotent and safe with a stale
    /// handle.
    pub fn close(&self, handle: u32) {
        self.table.close(handle);
        crate::service::DRAIN.ring();
    }

    /// Close every sink the guest opened: a guest's sinks last exactly as
    /// long as the instance that opened them (dsp_api.h).
    pub fn close_guests(&self) {
        let mut handles = [0u32; MAX_SINKS];
        let n = self.table.list(&mut handles).min(MAX_SINKS as u32) as usize;
        for &h in &handles[..n] {
            if self.acquire(h).is_some_and(|s| s.guest) { self.close(h); }
        }
    }

    /// Every open sink's handle, in slot order.
    ///
    /// # Safety
    /// `out` is valid for `cap` writes, or null when `cap` is 0.
    pub unsafe fn list(&self, out: *mut u32, cap: u32) -> u32 {
        // SAFETY: the contract above is `list_raw`'s.
        unsafe { self.table.list_raw(out, cap) }
    }

    /// How many sinks are open. What the drain thread uses to decide it has
    /// nothing left to do.
    pub fn open_count(&self) -> u32 {
        // SAFETY: null with a zero cap is the documented "count only" call.
        unsafe { self.table.list_raw(core::ptr::null_mut(), 0) }
    }

    /// Cancel everything the sink is holding. 0 if the handle is not live.
    pub fn flush(&self, handle: u32) -> u32 {
        self.acquire(handle).map(|s| s.flush()).unwrap_or(0)
    }

    /// Cancel everything EVERY open sink is holding, and say how many went.
    ///
    /// This is what a run-stop reaches for: one call, every port and every
    /// destination, whoever queued them. A guest that sent its own events
    /// with clockwork_sink_send is covered by the same call as a client that
    /// sent verbs — which is the point of putting cancellation on the sink
    /// rather than on whichever queue a message happened to pass through.
    pub fn flush_all(&self) -> u32 {
        // The table's own enumeration, the same one clockwork_sink_list uses
        // — a fixed-size buffer rather than an allocation, so this is
        // callable from anywhere a stop button can be pressed.
        let mut handles = [0u32; MAX_SINKS];
        let n_open = self.table.list(&mut handles) as usize;
        let mut n = 0;
        for &handle in handles.iter().take(n_open.min(MAX_SINKS)) {
            n += self.flush(handle);
        }
        n
    }

    /// Close every sink. For shutdown, and so one test case cannot leak slots
    /// into the next.
    pub fn close_all(&self) {
        self.table.close_all();
        crate::service::DRAIN.ring();
    }

    /// Drain one sink at `now`. Returns when it must next be pumped
    /// ([`Sink::pump`]).
    pub fn pump(&self, handle: u32, now: Timetag) -> Option<Timetag> {
        self.acquire(handle)?.pump(now)
    }

    /// Drain every sink the service is responsible for. Returns the soonest
    /// any of them must next be pumped.
    pub fn pump_service(&self, now: Timetag) -> Option<Timetag> {
        let mut handles = [0u32; MAX_SINKS];
        let n = self.table.list(&mut handles).min(MAX_SINKS as u32) as usize;
        let mut soonest: Option<Timetag> = None;
        for &h in &handles[..n] {
            let Some(sink) = self.acquire(h) else { continue };   // closed under us
            if sink.drive != Drive::Service { continue; }
            if let Some(w) = sink.pump(now) {
                soonest = Some(soonest.map_or(w, |s| s.min(w)));
            }
        }
        soonest
    }

    /// True if any service-driven sink has queued messages the drain has not
    /// taken. The drain checks this after it arms the doorbell and before it
    /// sleeps, so a send that lands in between still wakes it.
    pub fn any_queued(&self) -> bool {
        let mut handles = [0u32; MAX_SINKS];
        let n = self.table.list(&mut handles).min(MAX_SINKS as u32) as usize;
        handles[..n].iter().any(|&h| self.acquire(h).is_some_and(|s| s.drive == Drive::Service && s.has_queued()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::profile::default_for;

    /// A shape a host installs is its table's: it replaces the default for
    /// its kind only, and another table — another core's — keeps its own.
    #[test]
    fn an_installed_shape_replaces_the_default_for_its_kind_in_its_table_only() {
        const KIND: u32 = 77;
        let (mine, theirs) = (Sinks::new(core::ptr::null_mut()), Sinks::new(core::ptr::null_mut()));
        assert_eq!(mine.shape_for(KIND), default_for(KIND));
        assert!(mine.install_shape(KIND, vec![Class { cell_bytes: 32, cells: 4 }]));
        assert_eq!(mine.shape_for(KIND), Layout::single(32, 4));
        assert!(!mine.install_shape(KIND, vec![]), "a bad shape is refused…");
        assert_eq!(mine.shape_for(KIND), Layout::single(32, 4), "…and the good one stays");
        assert_eq!(mine.shape_for(KIND + 1), default_for(KIND + 1), "another kind is untouched");
        assert_eq!(theirs.shape_for(KIND), default_for(KIND), "another table is untouched");
    }

    /// The guest's sinks close with its instance, and nothing else does.
    #[test]
    fn closing_the_guests_closes_the_sinks_the_guest_opened_and_no_other() {
        use crate::endpoint::CaptureEndpoint;
        let t = Sinks::new(core::ptr::null_mut());
        let ep = || Arc::new(CaptureEndpoint::new()) as Arc<dyn Endpoint>;
        let guests = t.open_with(KIND_OSC, "a", 0, ep(), Drive::Manual, true);
        let ours = t.open_with(KIND_OSC, "b", 0, ep(), Drive::Manual, false);
        assert!(guests != 0 && ours != 0);
        t.close_guests();
        assert!(t.acquire(guests).is_none(), "the guest's sink closed");
        assert!(t.acquire(ours).is_some(), "clockwork's stayed open");
        t.close(ours);
    }
}
