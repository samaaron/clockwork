// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! `ScopeOut2`'s shared-memory slot claim and release.
//!
//! The ugen publishes audio into a ring in shared memory for the browser to
//! draw. Appending is the ugen's own business — it writes through
//! `shm_tap_stream_writer`, which has to stay next to the ugens. What lives
//! here is everything around that: which slot a unit gets, when a slot becomes
//! free again, and the counters a reader uses to notice either.
//!
//! Those rules are clockwork's, not the audio engine's, which is why they sit
//! apart from both engines and each one points its interface table at them.
//!
//! # The ownership rule
//!
//! This is the part that is not incidental. A slot can be re-claimed by a new
//! unit while a superseded one is still alive under an FX tail. That older
//! unit's late destructor must not mark the slot free underneath the new
//! writer: readers gate on the state, so the scope would grey out while audio
//! kept playing. Only the slot's current owner may free it.

use clockwork_fence::no_unwind;
use std::os::raw::{c_int, c_void};
use clockwork_abi::arena::{geom, ClockworkArenaHeader, ClockworkArenaRegion, CLOCKWORK_ARENA_MAGIC};
use portable_atomic::AtomicU64;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

/// The most slots any arena has (a core's layout refuses more). The real
/// count comes from the arena's table; this bounds the owner table, which
/// cannot be sized at run time without allocating on a path that must not
/// allocate.
const MAX_SLOTS: usize = clockwork_abi::arena::CLOCKWORK_TAP_SLOTS_MAX as usize;

/// Where the tap region is and how it is carved up.
///
/// Read from the arena's own table (`clockwork_arena.h`) rather than restated
/// here: the numbers change with the memory profile — an embedded build
/// shrinks the ring — and every reader of the arena reads the same table.
#[derive(Clone, Copy, Default)]
struct Geometry {
    /// Offset of the tap region from the base of shared memory.
    start: usize,
    /// Bytes of global header before the first slot.
    header_size: usize,
    /// Bytes per slot, header and ring together.
    slot_size: usize,
    /// How many slots this build has.
    max_taps: usize,
    /// Frames in one slot's ring.
    ring_frames: u32,
    /// Channels a slot's ring interleaves.
    channels: u32,
    /// The engine's track taps: the same slot shape, after the guest's slots
    /// in the same region. Zero on a build that carves none.
    track_slots: usize,
}

impl Geometry {
    /// Every slot a client can number: the guest's, then the track taps.
    fn slots(&self) -> usize {
        self.max_taps + self.track_slots
    }
}

/// The tap region's geometry, from the table at the front of the arena at
/// `base`: `None` for null, for memory that is not an arena, and for a table
/// with no tap region or one its own size cannot hold.
///
/// # Safety
/// `base` is null or the mapped base of an arena, at least as long as its
/// header.
unsafe fn geometry(base: *mut u8) -> Option<Geometry> {
    if base.is_null() {
        return None;
    }
    // SAFETY: an arena's header is at its base, per the contract; read only.
    let header = unsafe { &*base.cast::<ClockworkArenaHeader>() };
    if header.magic != CLOCKWORK_ARENA_MAGIC {
        return None;
    }
    let e = header.find(ClockworkArenaRegion::CLOCKWORK_TAPS)?;
    let max_taps = (e.geom[geom::TAP_SLOTS] as usize).min(MAX_SLOTS);
    let g = Geometry {
        start: e.offset as usize,
        header_size: e.geom[geom::TAP_HEADER_BYTES] as usize,
        slot_size: e.geom[geom::TAP_SLOT_BYTES] as usize,
        max_taps,
        ring_frames: e.geom[geom::TAP_RING_FRAMES],
        channels: e.geom[geom::TAP_CHANNELS],
        // Both kinds together must fit the owner table.
        track_slots: (e.geom[geom::TAP_TRACK_SLOTS] as usize).min(MAX_SLOTS - max_taps),
    };
    let laid_out = g.header_size.checked_add(g.slots().checked_mul(g.slot_size)?)?;
    // Taps when the region holds any slot: a profile may give the guest none
    // and keep the track taps.
    (g.slot_size > 0 && g.slots() > 0 && laid_out <= e.bytes as usize).then_some(g)
}

/// A core's tap slots: which handle owns each. Only the owner may free a
/// slot. The core holds one (clockwork-core), and hands it to the tap calls
/// with the arena they are in.
///
/// Written from the audio thread alone — slots are claimed and released in
/// unit constructors and destructors, which the engine runs there — but atomic
/// rather than plain, because a race here is silent rather than loud.
pub struct Taps {
    owners: [AtomicUsize; MAX_SLOTS],
}

impl Default for Taps {
    fn default() -> Self {
        Taps { owners: [const { AtomicUsize::new(0) }; MAX_SLOTS] }
    }
}

/// The header field offsets, from the tap region's start.
const HDR_ACTIVE_COUNT: usize = 4;
const HDR_VERSION: usize = 12;

/// The handle a scope ugen holds on a claimed tap slot.
///
/// Declared here rather than imported from the ugen ABI crate: that crate is
/// part of a guest's DSP surface and no guest lives in this repository. The
/// layout is the ABI, so it is repeated exactly — `internalData` first,
/// because the ugen tests a handle by that field and a mirror with the fields
/// the other way round hands back something that is never valid.
#[repr(C)]
#[allow(non_snake_case)]
pub struct TapBufferHnd {
    pub internalData: *mut c_void,
    pub data: *mut f32,
    pub channels: u32,
    pub maxFrames: u32,
}

/// A slot's header, at the start of every slot. The ring follows it.
///
/// Mirrors `shm_tap_stream`, whose header is asserted to be 32 bytes with
/// the ring 16-byte aligned after it.
#[repr(C, align(16))]
struct SlotHeader {
    /// 0 = free, 1 = active.
    state: AtomicU32,
    channels: u32,
    capacity_frames: u32,
    /// Times the slot has been claimed: bumped by every claim, before it
    /// publishes `state`, and never by a release. A reader polls, and a slot
    /// can be claimed and released between two polls; the count is how the
    /// reader knows. Wraps, so readers compare for change only.
    activations: AtomicU32,
    /// Total frames written since activation.
    write_position: AtomicU64,
    /// Engine sample position of frame 0 of this stream.
    base_engine_frames: AtomicU64,
}

const _: () = assert!(std::mem::size_of::<SlotHeader>() == 32);
const _: () = assert!(std::mem::offset_of!(SlotHeader, activations) == 12);

/// Claim slot `index` and format it for `channels` (zero, or more than the
/// ring interleaves, is all of them): every way a slot is claimed comes here,
/// so each counts as one live spell and moves the region's counters.
///
/// # Safety
/// As [`slot_ptr`]: `base` is the mapped region `g` describes and `index` is
/// inside it.
unsafe fn claim_slot(base: *mut u8, g: &Geometry, index: usize, channels: u32) -> *mut SlotHeader {
    let channels = if channels > 0 && channels <= g.channels { channels } else { g.channels };
    // SAFETY: per the contract.
    unsafe {
        let slot = slot_ptr(base, g, index);
        (*slot).channels = channels;
        (*slot).capacity_frames = g.ring_frames;
        (*slot).write_position.store(0, Ordering::Relaxed);
        (*slot).base_engine_frames.store(0, Ordering::Relaxed);
        // Counted before the state publishes it, so a reader that sees the
        // slot live sees this claim in the count.
        (*slot).activations.fetch_add(1, Ordering::Relaxed);
        (*slot).state.store(1, Ordering::Release);
        header_field(base, g, HDR_ACTIVE_COUNT).fetch_add(1, Ordering::Relaxed);
        header_field(base, g, HDR_VERSION).fetch_add(1, Ordering::Relaxed);
        slot
    }
}

/// A counter in the tap region's global header, for the caller's call.
///
/// # Safety
/// `base` must be the mapped arena the geometry describes, mapped while the
/// reference is used.
unsafe fn header_field<'a>(base: *mut u8, g: &Geometry, offset: usize) -> &'a AtomicU32 {
    // SAFETY: the header is at the region's start and the field offsets are
    // its layout; the caller keeps the arena mapped while it uses this.
    unsafe { &*base.add(g.start + offset).cast::<AtomicU32>() }
}

/// # Safety
/// `base` must be the mapped shared memory the geometry describes, and
/// `index` less than `g.slots()`: the guest's slots first, then the track
/// taps, as one index space.
unsafe fn slot_ptr(base: *mut u8, g: &Geometry, index: usize) -> *mut SlotHeader {
    // One region: the guest's slots, then the track taps, one stride apart.
    let at = g.start + g.header_size + index * g.slot_size;
    // SAFETY: the slot lies inside the region, per the contract.
    unsafe { base.add(at).cast::<SlotHeader>() }
}

/// Which slot a pointer is, when it lies in the region.
fn slot_index_of(base: *mut u8, g: &Geometry, slot: *const SlotHeader) -> Option<usize> {
    let p = slot as usize;
    let first = base as usize + g.start + g.header_size;
    let off = p.wrapping_sub(first);
    if off < g.slots() * g.slot_size && off % g.slot_size == 0 {
        return Some(off / g.slot_size);
    }
    None
}

/// Claim a slot for a unit, in its core's arena and owner table.
///
/// `max_frames` is ignored: a stream's capacity is fixed by the build, and the
/// argument survives only because the interface table's signature does.
///
/// # Safety
/// `hnd` must point at a live handle; `taps` is null or a core's; `arena` is
/// null or that core's mapped arena.
#[no_mangle]
pub unsafe extern "C" fn clockwork_tap_get(
    taps: *const Taps,
    arena: *mut c_void,
    index: c_int,
    channels: c_int,
    _max_frames: c_int,
    hnd: *mut TapBufferHnd,
) -> c_int {
    no_unwind(0, || {
        if hnd.is_null() {
            return 0;
        }
        // SAFETY: a live handle, per the contract.
        let h = unsafe { &mut *hnd };
        h.internalData = std::ptr::null_mut();

        // SAFETY: a core's, per the contract.
        let Some(taps) = (unsafe { taps.as_ref() }) else { return 0 };
        let base = arena.cast::<u8>();
        // SAFETY: null or the core's arena, per the contract.
        let Some(g) = (unsafe { geometry(base) }) else { return 0 };
        if index < 0 || index as usize >= g.slots() {
            return 0;
        }
        let index = index as usize;

        // A negative channel count is as unusable as zero: all of them.
        let channels = u32::try_from(channels).unwrap_or(0);
        // SAFETY: the index is inside the geometry the arena's table gives, and
        // the arena is mapped, per the contract.
        let slot = unsafe { claim_slot(base, &g, index, channels) };

        // The handle carries the slot's identity so release can check ownership,
        // and nothing else. `data` stays null: the legacy region-pointer contract
        // has no stream equivalent, and the ugen appends through its own writer.
        h.data = std::ptr::null_mut();
        h.internalData = slot.cast::<c_void>();
        h.channels = 0;
        h.maxFrames = 0;
        taps.owners[index].store(hnd as usize, Ordering::Relaxed);
        1
    })
}

/// Give a slot back, in the core it was claimed in.
///
/// # Safety
/// `hnd` must point at a live handle; `taps` and `arena` as
/// [`clockwork_tap_get`]'s.
#[no_mangle]
pub unsafe extern "C" fn clockwork_tap_release(taps: *const Taps, arena: *mut c_void, hnd: *mut TapBufferHnd) {
    no_unwind((), || {
        if hnd.is_null() {
            return;
        }
        // SAFETY: a live handle, per the contract.
        let h = unsafe { &mut *hnd };
        if h.internalData.is_null() {
            return;
        }
        let slot = h.internalData.cast::<SlotHeader>();
        let base = arena.cast::<u8>();

        // Which slot this is, when the region is still there to say. Integer
        // arithmetic, not pointer arithmetic: a handle can outlive the region it
        // was claimed in, and the point of the check is to find out whether the
        // slot is inside the region that exists now.
        // SAFETY: a core's, and null or its arena, per the contract.
        let (taps, g) = unsafe { (taps.as_ref(), geometry(base)) };
        let located = match (taps, g) {
            (Some(taps), Some(g)) => slot_index_of(base, &g, slot).map(|i| (i, g, taps)),
            _ => None,
        };

        // A slot outside the current region is not touched at all: the memory
        // it points into has been unmapped, or belongs to a region this claim
        // knew nothing about. The handle is nulled below either way.
        if let Some((i, g, taps)) = located {
            // Only the current owner may mark it free. A superseded unit's late
            // destructor — the previous run's scope node tearing down under an
            // FX kill-delay tail, after a re-run re-claimed the slot — would
            // otherwise store 0 under the live writer, and readers gate on
            // state == 1: the scope greys out forever while the audio plays on.
            let is_owner = taps.owners[i].load(Ordering::Relaxed) == hnd as usize;
            // SAFETY: the slot was just shown to lie inside the mapped region.
            unsafe {
                if is_owner {
                    (*slot).state.store(0, Ordering::Relaxed);
                    taps.owners[i].store(0, Ordering::Relaxed);
                    header_field(base, &g, HDR_VERSION).fetch_add(1, Ordering::Relaxed);
                }
                // Every claim incremented the active count, so every release
                // decrements it — owner or not — to keep the pairing balanced.
                let active = header_field(base, &g, HDR_ACTIVE_COUNT);
                if active.load(Ordering::Relaxed) > 0 {
                    active.fetch_sub(1, Ordering::Relaxed);
                }
            }
        }

        // Null the handle so a stale holder sees null rather than a dangling
        // pointer: the ugen's defensive checks rely on both being nullable.
        h.internalData = std::ptr::null_mut();
        h.data = std::ptr::null_mut();
    })
}

// ── A first-party writer ────────────────────────────────────────────────────
//
// Everything above serves the C++ `ScopeOut2`, which claims a slot through the
// interface table and then appends through its own C++ ring writer. A ugen
// written in Rust has neither, and cannot grow one: the ugen module forbids
// `unsafe`, so it cannot touch shared memory itself.
//
// So the same three operations are offered here as safe functions. The ring
// format is not re-derived — it is the one `shm_tap_stream::write` uses,
// because the JavaScript that draws the scope reads that format and does not
// care which language filled it.

/// The interleaved ring of slot `index`, and its shape.
///
/// # Safety
/// As [`slot_ptr`].
unsafe fn slot_ring(base: *mut u8, g: &Geometry, index: usize) -> (*mut f32, u32, u32) {
    // SAFETY: the slot is inside the region, per the contract, and the ring
    // is inline, immediately after the header fields.
    let (slot, data) = unsafe {
        let slot = slot_ptr(base, g, index);
        (slot, slot.cast::<u8>().add(core::mem::size_of::<SlotHeader>()).cast::<f32>())
    };
    // SAFETY: as above.
    let (mut cap, mut channels) = unsafe { ((*slot).capacity_frames, (*slot).channels) };
    if cap == 0 || cap > g.ring_frames {
        cap = g.ring_frames;
    }
    if channels == 0 || channels > g.channels {
        channels = g.channels;
    }
    (data, cap, channels)
}

/// Claim slot `index` for writing, at `channels` channels.
///
/// Returns false when there is no shared memory, or no such slot. Claiming an
/// already-claimed slot re-formats it, which is what a re-used scope number
/// needs and why the C++ side does the same.
///
/// # Safety
/// `base` is null or the base of a mapped arena, mapped for as long as this
/// call runs.
pub unsafe fn stream_activate(base: *mut u8, index: usize, channels: u32) -> bool {
    // SAFETY: null or an arena, per the contract.
    let Some(g) = (unsafe { geometry(base) }) else { return false };
    if index >= g.slots() {
        return false;
    }
    // SAFETY: the index is inside the geometry the arena's table gives.
    unsafe { claim_slot(base, &g, index, channels) };
    true
}

/// Append one block of interleaved audio to slot `index`.
///
/// `engine_frames` is the engine sample position of the first frame. On the
/// first write it anchors the stream; later it heals a discontinuity, so a
/// paused loop resumes at the right place on the display rather than drawing
/// its gap as audio.
///
/// # Safety
/// As [`stream_activate`].
pub unsafe fn stream_write(base: *mut u8, index: usize, interleaved: &[f32], frames: usize, engine_frames: u64) {
    if frames == 0 {
        return;
    }
    // SAFETY: null or an arena, per the contract.
    let Some(g) = (unsafe { geometry(base) }) else { return };
    if index >= g.slots() {
        return;
    }
    // SAFETY: as `stream_activate`; every index below is taken modulo the
    // slot's own clamped capacity, so it cannot leave the ring.
    unsafe {
        let slot = slot_ptr(base, &g, index);
        if (*slot).state.load(Ordering::Acquire) != 1 {
            return;
        }
        let (data, cap, channels) = slot_ring(base, &g, index);

        let mut pos = (*slot).write_position.load(Ordering::Relaxed);
        if pos == 0 {
            (*slot).base_engine_frames.store(engine_frames, Ordering::Relaxed);
        } else {
            let anchor = (*slot).base_engine_frames.load(Ordering::Relaxed);
            if engine_frames > anchor && engine_frames - anchor != pos {
                pos = engine_frames - anchor;
            }
        }

        let cap = cap as usize;
        let channels = channels as usize;
        let frames = frames.min(interleaved.len() / channels.max(1));
        let at = (pos % cap as u64) as usize;
        let first = (cap - at).min(frames);
        for f in 0..first {
            for c in 0..channels {
                *data.add((at + f) * channels + c) = interleaved[f * channels + c];
            }
        }
        for f in 0..frames - first {
            for c in 0..channels {
                *data.add(f * channels + c) = interleaved[(first + f) * channels + c];
            }
        }
        (*slot).write_position.store(pos + frames as u64, Ordering::Release);
    }
}

/// Give slot `index` back, so a reader stops drawing it.
///
/// # Safety
/// As [`stream_activate`].
pub unsafe fn stream_release(base: *mut u8, index: usize) {
    // SAFETY: null or an arena, per the contract.
    let Some(g) = (unsafe { geometry(base) }) else { return };
    if index >= g.slots() {
        return;
    }
    // SAFETY: as `stream_activate`.
    unsafe {
        let slot = slot_ptr(base, &g, index);
        if (*slot).state.swap(0, Ordering::AcqRel) == 1 {
            header_field(base, &g, HDR_ACTIVE_COUNT).fetch_sub(1, Ordering::Relaxed);
            header_field(base, &g, HDR_VERSION).fetch_add(1, Ordering::Relaxed);
        }
    }
}
