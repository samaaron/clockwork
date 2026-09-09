// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The port table: what sits in a slot, and the arguments a port refuses.
//!
//! # The slot discipline is not here any more
//!
//! `clockwork_ports.h` says a slot goes silent the instant it is closed, that a
//! closed slot must read as silence rather than fault, and that it is not
//! reused "until every reader has been told". That is a lifetime problem
//! solved without a lock on the read path, and it is solved in
//! `clockwork-slots` — one atomic word per slot carrying a generation, a
//! state and a refcount. The whole argument for it, and its own tests, live
//! there.
//!
//! It moved because `src/clockwork_event_sink.h` wants the identical rule for a
//! different payload: an event sink is also a stable slot, also closed from a
//! control thread while the audio thread is inside it, and also forbidden a
//! mutex on the send path. Two copies of that discipline would have been one
//! too many — the second is the copy that quietly loses a subtlety.
//!
//! What is left here is what is genuinely a PORT: the ring, the channel count,
//! the direction, the name, the two counters, and the argument checks that say
//! what a port may not be.

use std::ffi::CString;
use std::sync::atomic::{AtomicPtr, Ordering};
use portable_atomic::AtomicU64 as Counter;

use clockwork_slots::SlotTable;

use crate::ring::{round_capacity, FrameRing, MAX_CHANNELS};

/// The transfer signal's shape, matching `ClockworkPortSignal` in `src/clockwork_ports.h`.
pub type Signal = unsafe extern "C" fn(port: u32, ctx: *mut core::ffi::c_void);

/// Slots in the table. Stable indices, never compacted.
pub const MAX_PORTS: usize = 64;

/// Directions, matching `ClockworkPortDirection` in `src/clockwork_ports.h`.
pub const DIR_SOURCE: u32 = 1;
pub const DIR_SINK: u32 = 2;

pub struct Port {
    pub ring: FrameRing,
    pub channels: u32,
    pub direction: u32,
    /// Kept alive for `clockwork_port_name`, which hands out the pointer.
    pub name: CString,
    /// Frames the audio thread asked a source for and did not get.
    pub underruns: Counter,
    /// Frames the audio thread offered a sink that would not fit.
    pub overruns: Counter,
    /// Called on the audio thread after each transfer the audio thread makes
    /// (`clockwork_port_write` on a sink, `clockwork_port_read` on a source), or null.
    /// The function pointer is stored as a plain pointer so it can be swapped
    /// atomically; `ctx` is published first, so a signal that sees the
    /// function sees its context.
    pub signal: AtomicPtr<()>,
    pub signal_ctx: AtomicPtr<core::ffi::c_void>,
}

impl Port {
    /// Fire the transfer signal if one is installed. Audio thread; two loads.
    #[inline]
    pub fn fire_signal(&self, handle: u32) {
        let f = self.signal.load(Ordering::Acquire);
        if f.is_null() { return; }
        let ctx = self.signal_ctx.load(Ordering::Relaxed);
        // SAFETY: the pointer was stored from a `Signal` by `set_signal`, and
        // nothing else writes it, so the transmute recovers the fn it came
        // from.
        let f: Signal = unsafe { core::mem::transmute(f) };
        // SAFETY: the host's callback contract: `ctx` is whatever it
        // installed alongside the function.
        unsafe { f(handle, ctx) };
    }
}

/// A live reference to a port, with the slot's refcount held for its lifetime.
pub type Held = clockwork_slots::Held<'static, Port>;

// `SlotTable` in static storage: no allocation, no initialisation order,
// nothing to boot. The table exists before main and outlives it.
static PORTS: SlotTable<Port, MAX_PORTS> = SlotTable::new();

/// The table itself, for a caller that needs the slot diagnostics
/// (`slot_state`, `slot_refs`, `try_reclaim`) rather than a port.
pub fn table() -> &'static SlotTable<Port, MAX_PORTS> { &PORTS }

/// Take a reference on `handle`'s port, or `None` if the handle is stale, the
/// slot is closed, or the handle was never valid. Allocation-free.
#[inline]
pub fn acquire(handle: u32) -> Option<Held> {
    PORTS.acquire(handle)
}

/// Open a port. Allocates (the ring, the name); control thread only.
/// Returns 0 for a bad argument or a full table.
pub fn open(name: &str, direction: u32, channels: u32, capacity_frames: u32) -> u32 {
    if direction != DIR_SOURCE && direction != DIR_SINK { return 0; }
    if channels == 0 || channels > MAX_CHANNELS { return 0; }
    let cap = round_capacity(capacity_frames);

    let cname = match CString::new(name) {
        Ok(c) => c,
        // An interior NUL: the name is a C string and cannot carry one.
        Err(_) => CString::new("").unwrap(),
    };
    PORTS.open(move || Port {
        ring: FrameRing::new(cap, channels),
        channels,
        direction,
        name: cname,
        underruns: Counter::new(0),
        overruns: Counter::new(0),
        signal: AtomicPtr::new(core::ptr::null_mut()),
        signal_ctx: AtomicPtr::new(core::ptr::null_mut()),
    })
}

/// Open a port whose ring lives in memory the caller owns — a shared-memory
/// segment another process has mapped too. `capacity_frames` is NOT rounded:
/// both sides must agree on the geometry exactly, so a capacity that is not a
/// power of two is refused, as is storage smaller than `storage_bytes` says
/// the ring needs. See `FrameRing::in_storage` for `reset`.
///
/// # Safety
/// `storage` is valid for `storage_bytes` bytes, 64-byte aligned, and stays
/// mapped for as long as the port is open.
pub unsafe fn open_shared(
    name: &str, direction: u32, channels: u32, capacity_frames: u32,
    storage: *mut u8, storage_bytes: usize, reset: bool,
) -> u32 {
    if direction != DIR_SOURCE && direction != DIR_SINK { return 0; }
    if channels == 0 || channels > MAX_CHANNELS { return 0; }
    if capacity_frames != round_capacity(capacity_frames) { return 0; }
    if storage.is_null() || !(storage as usize).is_multiple_of(64) { return 0; }
    if storage_bytes < crate::ring::storage_bytes(capacity_frames, channels) { return 0; }
    let cname = CString::new(name).unwrap_or_else(|_| CString::new("").unwrap());
    // SAFETY: valid and mapped while open per the contract; aligned and big
    // enough by the two checks above.
    let ring = unsafe { FrameRing::in_storage(storage, capacity_frames, channels, reset) };
    PORTS.open(move || Port {
        ring,
        channels,
        direction,
        name: cname,
        underruns: Counter::new(0),
        overruns: Counter::new(0),
        signal: AtomicPtr::new(core::ptr::null_mut()),
        signal_ctx: AtomicPtr::new(core::ptr::null_mut()),
    })
}

/// Install (or with `None`, remove) the port's transfer signal. Control
/// thread. The context goes first so the audio thread, which loads the
/// function with acquire, never sees a function without its context. Returns
/// false for a handle that names no open port.
pub fn set_signal(handle: u32, f: Option<Signal>, ctx: *mut core::ffi::c_void) -> bool {
    let Some(held) = acquire(handle) else { return false };
    let p = held.get();
    match f {
        Some(f) => {
            p.signal_ctx.store(ctx, Ordering::Relaxed);
            p.signal.store(f as *mut (), Ordering::Release);
        }
        None => {
            p.signal.store(core::ptr::null_mut(), Ordering::Release);
        }
    }
    true
}

/// Close a port: silent immediately, freed as soon as nobody is inside.
/// Idempotent, and safe against a stale or never-valid handle.
pub fn close(handle: u32) {
    PORTS.close(handle);
}

/// Every open slot's handle, in slot order. Writes at most `cap`, returns how
/// many exist. No allocation.
///
/// # Safety
/// `out` is valid for `cap` writes, or null when `cap` is 0.
pub unsafe fn list(out: *mut u32, cap: u32) -> u32 {
    // SAFETY: the contract above is `list_raw`'s.
    unsafe { PORTS.list_raw(out, cap) }
}

// ── The two sides, in Rust ──────────────────────────────────────────────────
//
// What the C ABI in ffi.rs does once it has turned pointers into slices. A
// Rust caller (clockwork-ports-disk, a test) comes in here and never touches
// a raw pointer.

/// The endpoint's side of a source: offer interleaved frames. Returns how
/// many were taken; a short return is the ring being full, not an overrun —
/// the producer keeps the rest and tries again.
pub fn produce(port: u32, interleaved: &[f32]) -> u32 {
    let Some(held) = acquire(port) else { return 0 };
    let p = held.get();
    if p.direction != DIR_SOURCE { return 0; }
    p.ring.push_interleaved(interleaved) as u32
}

/// The endpoint's side of a sink: take interleaved frames. Returns how many
/// there were.
pub fn consume(port: u32, interleaved: &mut [f32]) -> u32 {
    let Some(held) = acquire(port) else { return 0 };
    let p = held.get();
    if p.direction != DIR_SINK { return 0; }
    p.ring.pop_interleaved(interleaved) as u32
}

/// The audio thread's side of a source: fill planar buffers, zero-filling
/// whatever is not there, counting the shortfall as underrun, and firing the
/// transfer signal. A port that is not a live source gives silence.
pub fn read(port: u32, out: &mut [Option<&mut [f32]>], frames: usize) -> u32 {
    let Some(held) = acquire(port) else { zero_planar(out, frames); return 0 };
    let p = held.get();
    if p.direction != DIR_SOURCE { zero_planar(out, frames); return 0; }
    let got = p.ring.pop_planar(out, frames) as u32;
    if (got as usize) < frames {
        // The shortfall, not the block: a port that delivers half a block is
        // half an underrun, and rounding that up to a whole one would make the
        // number useless for judging whether a stream is nearly keeping up.
        p.underruns.fetch_add(frames as u64 - got as u64, Ordering::Relaxed);
    }
    p.fire_signal(port);
    got
}

/// The audio thread's side of a sink: offer planar buffers, counting what
/// does not fit as overrun, and firing the transfer signal.
pub fn write(port: u32, input: &[Option<&[f32]>], frames: usize) -> u32 {
    let Some(held) = acquire(port) else { return 0 };
    let p = held.get();
    if p.direction != DIR_SINK { return 0; }
    let took = p.ring.push_planar(input, frames) as u32;
    if (took as usize) < frames {
        p.overruns.fetch_add(frames as u64 - took as u64, Ordering::Relaxed);
    }
    p.fire_signal(port);
    took
}

/// Silence into every buffer offered, out to `frames`: what a caller gets
/// from a port that is not there, so its output is never whatever the
/// buffer held before.
fn zero_planar(out: &mut [Option<&mut [f32]>], frames: usize) {
    for d in out.iter_mut().flatten() {
        let end = frames.min(d.len());
        for v in &mut d[..end] { *v = 0.0; }
    }
}

pub fn channels(port: u32) -> u32 { acquire(port).map_or(0, |h| h.get().channels) }
pub fn writable(port: u32) -> u32 { acquire(port).map_or(0, |h| h.get().ring.writable() as u32) }
pub fn readable(port: u32) -> u32 { acquire(port).map_or(0, |h| h.get().ring.readable() as u32) }

/// Close every open port. For tests and for shutdown; not on any hot path.
pub fn close_all() {
    PORTS.close_all();
}

/// The table is one static, and the test harness runs tests in parallel: a
/// test that calls `close_all` closes the ports of whichever test is running
/// beside it. Every test that touches the table holds this for its duration.
#[cfg(test)]
pub(crate) fn table_lock() -> std::sync::MutexGuard<'static, ()> {
    static LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_slots::SlotState;

    /// The discipline itself is `clockwork-slots`'s, and is tested there.
    /// What is worth asserting HERE is that the port table is actually wired
    /// to it — that `clockwork_ports.h`'s promise is in force for ports, rather than
    /// merely available in a crate ports depends on.
    #[test]
    fn a_closed_port_stays_reserved_while_a_reader_is_inside_it() {
        let _t = table_lock();
        close_all();
        let h = open("held", DIR_SOURCE, 2, 64);
        assert_ne!(h, 0);
        let held = acquire(h).expect("open port must be enterable");

        close(h);
        assert!(acquire(h).is_none(), "a closed port must not admit a reader");
        assert_eq!(table().slot_state(h), Some(SlotState::Closed));
        assert_eq!(held.get().channels, 2, "the port behind the guard is intact");

        drop(held);
        assert!(table().try_reclaim(table().slot_index(h).unwrap()));
        assert_eq!(table().slot_state(h), Some(SlotState::Free));
    }

    /// The argument checks are the port's own, and none of them is in the
    /// slot table: a direction that is neither, no channels, or more channels
    /// than a ring will hold.
    #[test]
    fn a_port_refuses_what_it_cannot_be() {
        let _t = table_lock();
        close_all();
        assert_eq!(open("no-direction", 0, 1, 16), 0);
        assert_eq!(open("no-channels", DIR_SOURCE, 0, 16), 0);
        assert_eq!(open("too-many", DIR_SOURCE, MAX_CHANNELS + 1, 16), 0);
        let ok = open("fine", DIR_SINK, 2, 16);
        assert_ne!(ok, 0);
        close(ok);
    }

    /// A shared port is exact about its geometry, because the other side of
    /// the memory will have laid it out by the same numbers: a capacity that
    /// would be rounded, or storage too small for it, is refused rather than
    /// quietly reshaped.
    #[test]
    fn a_shared_port_refuses_a_geometry_the_other_side_would_not_share() {
        let _t = table_lock();
        #[repr(align(64))]
        struct Aligned([u8; 128 + 64 * 2 * 4]);
        let mut mem = Aligned([0u8; 128 + 64 * 2 * 4]);
        let base = mem.0.as_mut_ptr();
        let bytes = mem.0.len();
        // SAFETY: `mem` is 64-aligned and `bytes` long; `base.add(4)` is
        // still inside it, and the port opened on it is closed before `mem`
        // goes away.
        unsafe {
            assert_eq!(open_shared("odd", DIR_SINK, 2, 60, base, bytes, true), 0);
            assert_eq!(open_shared("small", DIR_SINK, 2, 64, base, bytes - 1, true), 0);
            assert_eq!(open_shared("misaligned", DIR_SINK, 2, 64, base.add(4), bytes, true), 0);
            let h = open_shared("fine", DIR_SINK, 2, 64, base, bytes, true);
            assert_ne!(h, 0);
            assert_eq!(acquire(h).unwrap().get().ring.capacity_frames(), 64);
            close(h);
        }
    }

    /// The signal fires with the context it was installed with, and removing
    /// it stops the firing. The audio thread's half of this — that it is
    /// called after a transfer — is exercised through the FFI in `ffi.rs`.
    #[test]
    fn a_signal_carries_its_context_and_can_be_removed() {
        let _t = table_lock();
        static HITS: Counter = Counter::new(0);
        unsafe extern "C" fn bump(port: u32, ctx: *mut core::ffi::c_void) {
            assert_eq!(ctx as usize, 0x1234);
            HITS.fetch_add(port as u64, Ordering::Relaxed);
        }
        let h = open("signalled", DIR_SINK, 1, 16);
        assert!(set_signal(h, Some(bump), 0x1234 as *mut _));
        acquire(h).unwrap().get().fire_signal(h);
        assert_eq!(HITS.load(Ordering::Relaxed), h as u64);
        assert!(set_signal(h, None, core::ptr::null_mut()));
        acquire(h).unwrap().get().fire_signal(h);
        assert_eq!(HITS.load(Ordering::Relaxed), h as u64, "removed: not fired again");
        assert!(!set_signal(0, Some(bump), core::ptr::null_mut()));
        close(h);
    }
}
