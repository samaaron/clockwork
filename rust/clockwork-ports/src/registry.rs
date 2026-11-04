// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! The port table: what sits in a slot, and the arguments a port refuses.
//!
//! # The slot discipline is in `clockwork-slots`
//!
//! `clockwork_ports.h` says a slot goes silent the instant it is closed, that a
//! closed slot must read as silence rather than fault, and that it is not
//! reused "until every reader has been told". That is a lifetime problem
//! solved without a lock on the read path, and it is solved in
//! `clockwork-slots` — one atomic word per slot carrying a generation, a
//! state and a refcount. The whole argument for it, and its own tests, live
//! there.
//!
//! It is shared because `src/clockwork_event_sink.h` wants the identical rule
//! for a different payload: an event sink is also a stable slot, also closed
//! from a control thread while the audio thread is inside it, and also
//! forbidden a mutex on the send path. One copy serves both.
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
pub type Held<'a> = clockwork_slots::Held<'a, Port>;

/// Memory the caller owns that a shared port's ring lives in
/// ([`Ports::open_shared`]): a segment another process has mapped too.
pub struct SharedStorage {
    /// 64-byte aligned, valid for `bytes`, mapped for as long as the port
    /// is open.
    pub base: *mut u8,
    pub bytes: usize,
    /// Lay the ring out afresh — the side that made the memory — or join it
    /// as it stands (`FrameRing::in_storage`).
    pub reset: bool,
}

/// A table of ports: a core's (clockwork-core), or a host's own — a plugin
/// bridge's, a test's. A port's handle names a slot in the table it was
/// opened in and in no other. No allocation to make one, nothing to boot.
pub struct Ports {
    table: SlotTable<Port, MAX_PORTS>,
}

impl Default for Ports {
    fn default() -> Self { Ports::new() }
}

// What a table still holds when it goes is closed with it, so nothing open
// in it outlives it.
impl Drop for Ports {
    fn drop(&mut self) { self.table.close_all(); }
}

impl Ports {
    pub const fn new() -> Ports { Ports { table: SlotTable::new() } }

    /// The port's ring, in frames. 0 if the port is not open.
    pub fn capacity(&self, port: u32) -> u32 {
        self.acquire(port).map_or(0, |h| h.get().ring.capacity_frames() as u32)
    }

    /// The table itself, for a caller that needs the slot diagnostics
    /// (`slot_state`, `slot_refs`) rather than a port.
    pub fn table(&self) -> &SlotTable<Port, MAX_PORTS> { &self.table }

    /// Take a reference on `handle`'s port, or `None` if the handle is stale, the
    /// slot is closed, or the handle was never valid. Allocation-free.
    #[inline]
    pub fn acquire(&self, handle: u32) -> Option<Held<'_>> {
        self.table.acquire(handle)
    }

    /// Open a port. Allocates (the ring, the name); control thread only.
    /// Returns 0 for a bad argument or a full table.
    pub fn open(&self, name: &str, direction: u32, channels: u32, capacity_frames: u32) -> u32 {
        if direction != DIR_SOURCE && direction != DIR_SINK { return 0; }
        if channels == 0 || channels > MAX_CHANNELS { return 0; }
        let cap = round_capacity(capacity_frames);

        let cname = match CString::new(name) {
            Ok(c) => c,
            // An interior NUL: the name is a C string and cannot carry one.
            Err(_) => CString::new("").unwrap(),
        };
        self.table.open(move || Port {
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
    /// power of two is refused, as is storage smaller than the ring needs.
    ///
    /// # Safety
    /// `storage` is as [`SharedStorage`] says.
    pub unsafe fn open_shared(
        &self, name: &str, direction: u32, channels: u32, capacity_frames: u32, storage: SharedStorage,
    ) -> u32 {
        if direction != DIR_SOURCE && direction != DIR_SINK { return 0; }
        if channels == 0 || channels > MAX_CHANNELS { return 0; }
        if capacity_frames != round_capacity(capacity_frames) { return 0; }
        if storage.base.is_null() || (storage.base as usize) % 64 != 0 { return 0; }
        if storage.bytes < crate::ring::storage_bytes(capacity_frames, channels) { return 0; }
        let cname = CString::new(name).unwrap_or_else(|_| CString::new("").unwrap());
        // SAFETY: valid and mapped while open per the contract; aligned and big
        // enough by the two checks above.
        let ring = unsafe { FrameRing::in_storage(storage.base, capacity_frames, channels, storage.reset) };
        self.table.open(move || Port {
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
    pub fn set_signal(&self, handle: u32, f: Option<Signal>, ctx: *mut core::ffi::c_void) -> bool {
        let Some(held) = self.acquire(handle) else { return false };
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

    /// Close a port. It goes silent at once, and the call returns once nobody is
    /// inside it, including the audio thread's transfer and its signal.
    /// Idempotent, and safe against a stale or never-valid handle.
    pub fn close(&self, handle: u32) {
        self.table.close(handle);
    }

    /// Every open slot's handle, in slot order. Writes at most `cap`, returns how
    /// many exist. No allocation.
    ///
    /// # Safety
    /// `out` is valid for `cap` writes, or null when `cap` is 0.
    pub unsafe fn list(&self, out: *mut u32, cap: u32) -> u32 {
        // SAFETY: the contract above is `list_raw`'s.
        unsafe { self.table.list_raw(out, cap) }
    }

    // ── The two sides, in Rust ──────────────────────────────────────────────────
    //
    // What the C ABI in ffi.rs does once it has turned pointers into slices. A
    // Rust caller (clockwork-ports-disk, a test) comes in here and never touches
    // a raw pointer.

    /// The endpoint's side of a source: offer interleaved frames. Returns how
    /// many were taken; a short return is the ring being full, not an overrun —
    /// the producer keeps the rest and tries again.
    pub fn produce(&self, port: u32, interleaved: &[f32]) -> u32 {
        let Some(held) = self.acquire(port) else { return 0 };
        let p = held.get();
        if p.direction != DIR_SOURCE { return 0; }
        p.ring.push_interleaved(interleaved) as u32
    }

    /// The endpoint's side of a sink: take interleaved frames. Returns how many
    /// there were.
    pub fn consume(&self, port: u32, interleaved: &mut [f32]) -> u32 {
        let Some(held) = self.acquire(port) else { return 0 };
        let p = held.get();
        if p.direction != DIR_SINK { return 0; }
        p.ring.pop_interleaved(interleaved) as u32
    }

    /// The audio thread's side of a source: fill planar buffers, zero-filling
    /// whatever is not there, counting the shortfall as underrun, and firing the
    /// transfer signal. A port that is not a live source gives silence.
    pub fn read(&self, port: u32, out: &mut [Option<&mut [f32]>], frames: usize) -> u32 {
        let Some(held) = self.acquire(port) else { zero_planar(out, frames); return 0 };
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
    pub fn write(&self, port: u32, input: &[Option<&[f32]>], frames: usize) -> u32 {
        let Some(held) = self.acquire(port) else { return 0 };
        let p = held.get();
        if p.direction != DIR_SINK { return 0; }
        let took = p.ring.push_planar(input, frames) as u32;
        if (took as usize) < frames {
            p.overruns.fetch_add(frames as u64 - took as u64, Ordering::Relaxed);
        }
        p.fire_signal(port);
        took
    }


    pub fn channels(&self, port: u32) -> u32 { self.acquire(port).map_or(0, |h| h.get().channels) }
    pub fn writable(&self, port: u32) -> u32 { self.acquire(port).map_or(0, |h| h.get().ring.writable() as u32) }
    pub fn readable(&self, port: u32) -> u32 { self.acquire(port).map_or(0, |h| h.get().ring.readable() as u32) }

    /// Close every open port. For tests and for shutdown; not on any hot path.
    pub fn close_all(&self) {
        self.table.close_all();
    }
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

#[cfg(test)]
mod tests {
    use super::*;
    use clockwork_slots::SlotState;

    /// The discipline itself is `clockwork-slots`'s, and is tested there.
    /// What is worth asserting HERE is that the port table is actually wired
    /// to it — that `clockwork_ports.h`'s promise is in force for ports, rather than
    /// merely available in a crate ports depends on.
    #[test]
    fn a_close_returns_only_once_the_audio_thread_is_out_of_the_port_and_its_signal() {
        use std::sync::atomic::AtomicBool;
        let ports = Ports::new();
        let h = ports.open("held", DIR_SOURCE, 2, 64);
        assert_ne!(h, 0);

        // The audio thread is mid-transfer: inside the port, in its signal,
        // held there until the case lets it go. A close that returned now
        // would free what the signal is using, such as the context a host
        // frees after closing or the memory a shared port lives in.
        static IN_SIGNAL: AtomicBool = AtomicBool::new(false);
        static LET_GO: AtomicBool = AtomicBool::new(false);
        unsafe extern "C" fn held_signal(_: u32, _: *mut core::ffi::c_void) {
            IN_SIGNAL.store(true, Ordering::SeqCst);
            while !LET_GO.load(Ordering::SeqCst) { std::thread::yield_now(); }
        }
        IN_SIGNAL.store(false, Ordering::SeqCst);
        LET_GO.store(false, Ordering::SeqCst);
        assert!(ports.set_signal(h, Some(held_signal), core::ptr::null_mut()));

        // Lets the signal go however the case ends, so a failed check fails
        // rather than leaving the audio thread in its signal for ever.
        struct LetGoOnDrop;
        impl Drop for LetGoOnDrop {
            fn drop(&mut self) { LET_GO.store(true, Ordering::SeqCst); }
        }

        let closed = AtomicBool::new(false);
        std::thread::scope(|s| {
            let let_go = LetGoOnDrop;
            s.spawn(|| {
                let (mut l, mut r) = ([0.0f32; 16], [0.0f32; 16]);
                ports.read(h, &mut [Some(&mut l[..]), Some(&mut r[..])], 16);
            });
            while !IN_SIGNAL.load(Ordering::SeqCst) { std::thread::yield_now(); }
            let closer = s.spawn(|| {
                ports.close(h);
                closed.store(true, Ordering::SeqCst);
            });
            // The slot is shut at once to anyone new, and reserved, not
            // freed, while the signal runs.
            while ports.table().slot_state(h) != Some(SlotState::Closed) { std::thread::yield_now(); }
            assert!(ports.acquire(h).is_none(), "a closed port must not admit a reader");
            std::thread::sleep(std::time::Duration::from_millis(20));
            assert!(!closed.load(Ordering::SeqCst), "the close returned with the signal still running");
            assert_eq!(ports.table().slot_state(h), Some(SlotState::Closed));

            drop(let_go);
            closer.join().unwrap();
            assert!(closed.load(Ordering::SeqCst));
            assert_eq!(ports.table().slot_state(h), Some(SlotState::Free));
        });
    }

    /// The argument checks are the port's own, and none of them is in the
    /// slot table: a direction that is neither, no channels, or more channels
    /// than a ring will hold.
    #[test]
    fn a_port_refuses_what_it_cannot_be() {
        let ports = Ports::new();
        assert_eq!(ports.open("no-direction", 0, 1, 16), 0);
        assert_eq!(ports.open("no-channels", DIR_SOURCE, 0, 16), 0);
        assert_eq!(ports.open("too-many", DIR_SOURCE, MAX_CHANNELS + 1, 16), 0);
        let ok = ports.open("fine", DIR_SINK, 2, 16);
        assert_ne!(ok, 0);
        ports.close(ok);
    }

    /// A shared port is exact about its geometry, because the other side of
    /// the memory will have laid it out by the same numbers: a capacity that
    /// would be rounded, or storage too small for it, is refused rather than
    /// quietly reshaped.
    #[test]
    fn a_shared_port_refuses_a_geometry_the_other_side_would_not_share() {
        let ports = Ports::new();
        #[repr(align(64))]
        struct Aligned([u8; 128 + 64 * 2 * 4]);
        let mut mem = Aligned([0u8; 128 + 64 * 2 * 4]);
        let base = mem.0.as_mut_ptr();
        let bytes = mem.0.len();
        // SAFETY: `mem` is 64-aligned and `bytes` long; `base.add(4)` is
        // still inside it, and the port opened on it is closed before `mem`
        // goes away.
        unsafe {
            assert_eq!(ports.open_shared("odd", DIR_SINK, 2, 60, SharedStorage { base, bytes, reset: true }), 0);
            assert_eq!(ports.open_shared("small", DIR_SINK, 2, 64, SharedStorage { base, bytes: bytes - 1, reset: true }), 0);
            assert_eq!(ports.open_shared("misaligned", DIR_SINK, 2, 64, SharedStorage { base: base.add(4), bytes, reset: true }), 0);
            let h = ports.open_shared("fine", DIR_SINK, 2, 64, SharedStorage { base, bytes, reset: true });
            assert_ne!(h, 0);
            assert_eq!(ports.acquire(h).unwrap().get().ring.capacity_frames(), 64);
            ports.close(h);
        }
    }

    /// The signal fires with the context it was installed with, and removing
    /// it stops the firing. The audio thread's half of this — that it is
    /// called after a transfer — is exercised through the FFI in `ffi.rs`.
    #[test]
    fn a_signal_carries_its_context_and_can_be_removed() {
        let ports = Ports::new();
        static HITS: Counter = Counter::new(0);
        unsafe extern "C" fn bump(port: u32, ctx: *mut core::ffi::c_void) {
            assert_eq!(ctx as usize, 0x1234);
            HITS.fetch_add(port as u64, Ordering::Relaxed);
        }
        let h = ports.open("signalled", DIR_SINK, 1, 16);
        assert!(ports.set_signal(h, Some(bump), 0x1234 as *mut _));
        ports.acquire(h).unwrap().get().fire_signal(h);
        assert_eq!(HITS.load(Ordering::Relaxed), h as u64);
        assert!(ports.set_signal(h, None, core::ptr::null_mut()));
        ports.acquire(h).unwrap().get().fire_signal(h);
        assert_eq!(HITS.load(Ordering::Relaxed), h as u64, "removed: not fired again");
        assert!(!ports.set_signal(0, Some(bump), core::ptr::null_mut()));
        ports.close(h);
    }
}
