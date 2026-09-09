// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//! `ScopeOut2`'s shared-memory slot claim and release.
//!
//! The ugen publishes audio into a ring in shared memory for the browser to
//! draw. Appending is the ugen's own business — it writes through
//! `shm_scope_stream_writer`, which has to stay next to the ugens. What lives
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

use std::os::raw::{c_int, c_void};
use portable_atomic::AtomicU64;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

/// The most slots any build has. The real count comes from the host and is
/// never larger than this; it bounds the owner table, which cannot be sized at
/// run time without allocating on a path that must not allocate.
const MAX_SLOTS: usize = 64;

/// Where the scope region is and how it is carved up.
///
/// Read from the host rather than restated here: the numbers are build-time
/// constants that change with the memory profile — an embedded build shrinks
/// the ring — and JavaScript reads the same layout. Three copies of a constant
/// is two too many.
#[derive(Clone, Copy, Default)]
struct Geometry {
    /// Offset of the scope region from the base of shared memory.
    start: usize,
    /// Bytes of global header before the first slot.
    header_size: usize,
    /// Bytes per slot, header and ring together.
    slot_size: usize,
    /// How many slots this build has.
    max_scopes: usize,
    /// Frames in one slot's ring.
    ring_frames: u32,
    /// Channels a slot's ring interleaves.
    channels: u32,
    /// The engine's track taps: the same slot shape, in the clockwork block,
    /// numbered after the guest's. Zero slots on a build that carves none.
    track_start: usize,
    track_slots: usize,
}

impl Geometry {
    /// Every slot a client can number: the guest's, then the track taps.
    fn slots(&self) -> usize {
        self.max_scopes + self.track_slots
    }
}

extern "C" {
    /// The base of shared memory, or null before it exists. Asked for on every
    /// call rather than cached: the segment is created and destroyed with the
    /// engine, and a stale base is a wild pointer.
    fn clockwork_shm_base() -> *mut c_void;
    /// The scope region's geometry, filled by the host. Returns zero if the
    /// build has no scope region.
    fn clockwork_scope_geometry(
        start: *mut usize,
        header_size: *mut usize,
        slot_size: *mut usize,
        max_scopes: *mut usize,
        ring_frames: *mut u32,
        channels: *mut u32,
        track_start: *mut usize,
        track_slots: *mut usize,
    ) -> c_int;
}

/// The geometry, fetched once. Zero means "not yet asked".
static GEOMETRY_CACHE: [AtomicUsize; 8] = [const { AtomicUsize::new(0) }; 8];
static GEOMETRY_KNOWN: AtomicU32 = AtomicU32::new(0);

fn geometry() -> Option<Geometry> {
    if GEOMETRY_KNOWN.load(Ordering::Acquire) == 0 {
        let (mut start, mut header_size, mut slot_size, mut max_scopes) = (0usize, 0, 0, 0);
        let (mut ring_frames, mut channels) = (0u32, 0u32);
        let (mut track_start, mut track_slots) = (0usize, 0usize);
        // SAFETY: eight live out-pointers, which is all the host's function
        // asks for.
        let ok = unsafe {
            clockwork_scope_geometry(
                &mut start,
                &mut header_size,
                &mut slot_size,
                &mut max_scopes,
                &mut ring_frames,
                &mut channels,
                &mut track_start,
                &mut track_slots,
            )
        };
        if ok == 0 || slot_size == 0 || max_scopes == 0 {
            return None;
        }
        GEOMETRY_CACHE[0].store(start, Ordering::Relaxed);
        GEOMETRY_CACHE[1].store(header_size, Ordering::Relaxed);
        GEOMETRY_CACHE[2].store(slot_size, Ordering::Relaxed);
        GEOMETRY_CACHE[3].store(max_scopes.min(MAX_SLOTS), Ordering::Relaxed);
        GEOMETRY_CACHE[4].store(ring_frames as usize, Ordering::Relaxed);
        GEOMETRY_CACHE[5].store(channels as usize, Ordering::Relaxed);
        GEOMETRY_CACHE[6].store(track_start, Ordering::Relaxed);
        // Both spans together must fit the owner table.
        GEOMETRY_CACHE[7].store(track_slots.min(MAX_SLOTS - max_scopes.min(MAX_SLOTS)), Ordering::Relaxed);
        GEOMETRY_KNOWN.store(1, Ordering::Release);
    }
    Some(Geometry {
        start: GEOMETRY_CACHE[0].load(Ordering::Relaxed),
        header_size: GEOMETRY_CACHE[1].load(Ordering::Relaxed),
        slot_size: GEOMETRY_CACHE[2].load(Ordering::Relaxed),
        max_scopes: GEOMETRY_CACHE[3].load(Ordering::Relaxed),
        ring_frames: GEOMETRY_CACHE[4].load(Ordering::Relaxed) as u32,
        channels: GEOMETRY_CACHE[5].load(Ordering::Relaxed) as u32,
        track_start: GEOMETRY_CACHE[6].load(Ordering::Relaxed),
        track_slots: GEOMETRY_CACHE[7].load(Ordering::Relaxed),
    })
}

/// Which handle currently owns each slot. Only the owner may free it.
///
/// Written from the audio thread alone — slots are claimed and released in
/// unit constructors and destructors, which the engine runs there — but atomic
/// rather than plain, because a cross-process reader may look at the same
/// memory and because a race here is silent rather than loud.
static SLOT_OWNERS: [AtomicUsize; MAX_SLOTS] = [const { AtomicUsize::new(0) }; MAX_SLOTS];

/// The header field offsets, from the scope region's start.
const HDR_ACTIVE_COUNT: usize = 4;
const HDR_VERSION: usize = 12;

/// The handle a scope ugen holds on a claimed scope slot.
///
/// Declared here rather than imported from the ugen ABI crate: that crate is
/// part of a guest's DSP surface and no guest lives in this repository. The
/// layout is the ABI, so it is repeated exactly — `internalData` first,
/// because the ugen tests a handle by that field and a mirror with the fields
/// the other way round hands back something that is never valid.
#[repr(C)]
#[allow(non_snake_case)]
pub struct ScopeBufferHnd {
    pub internalData: *mut c_void,
    pub data: *mut f32,
    pub channels: u32,
    pub maxFrames: u32,
}

/// A slot's header, at the start of every slot. The ring follows it.
///
/// Mirrors `shm_scope_stream`, whose header is asserted to be 32 bytes with
/// the ring 16-byte aligned after it.
#[repr(C, align(16))]
struct SlotHeader {
    /// 0 = free, 1 = active.
    state: AtomicU32,
    channels: u32,
    capacity_frames: u32,
    _pad0: u32,
    /// Total frames written since activation.
    write_position: AtomicU64,
    /// Engine sample position of frame 0 of this stream.
    base_engine_frames: AtomicU64,
}

const _: () = assert!(std::mem::size_of::<SlotHeader>() == 32);

/// A counter in the scope region's global header.
///
/// # Safety
/// `base` must be the mapped shared memory the geometry describes.
unsafe fn header_field(base: *mut u8, g: &Geometry, offset: usize) -> &'static AtomicU32 {
    // SAFETY: the header is at the region's start and the field offsets are
    // its layout; the region is mapped for the life of the process, so the
    // reference cannot outlive it.
    unsafe { &*base.add(g.start + offset).cast::<AtomicU32>() }
}

/// # Safety
/// `base` must be the mapped shared memory the geometry describes, and
/// `index` less than `g.slots()`: the guest's slots first, then the track
/// taps, as one index space.
unsafe fn slot_ptr(base: *mut u8, g: &Geometry, index: usize) -> *mut SlotHeader {
    let at = if index < g.max_scopes {
        g.start + g.header_size + index * g.slot_size
    } else {
        g.track_start + (index - g.max_scopes) * g.slot_size
    };
    // SAFETY: the slot lies inside one of the two regions, per the contract.
    unsafe { base.add(at).cast::<SlotHeader>() }
}

/// Which slot a pointer is, when it lies in either span of the geometry.
fn slot_index_of(base: *mut u8, g: &Geometry, slot: *const SlotHeader) -> Option<usize> {
    let p = slot as usize;
    let guest = base as usize + g.start + g.header_size;
    let off = p.wrapping_sub(guest);
    if off < g.max_scopes * g.slot_size && off.is_multiple_of(g.slot_size) {
        return Some(off / g.slot_size);
    }
    let track = base as usize + g.track_start;
    let off = p.wrapping_sub(track);
    if g.track_slots > 0 && off < g.track_slots * g.slot_size && off.is_multiple_of(g.slot_size) {
        return Some(g.max_scopes + off / g.slot_size);
    }
    None
}

/// Claim a slot for a unit.
///
/// `max_frames` is ignored: a stream's capacity is fixed by the build, and the
/// argument survives only because the interface table's signature does.
///
/// # Safety
/// `hnd` must point at a live handle.
#[no_mangle]
pub unsafe extern "C" fn clockwork_scope_get(
    _ctx: *mut c_void,
    index: c_int,
    channels: c_int,
    _max_frames: c_int,
    hnd: *mut ScopeBufferHnd,
) -> c_int {
    if hnd.is_null() {
        return 0;
    }
    // SAFETY: a live handle, per the contract.
    let h = unsafe { &mut *hnd };
    h.internalData = std::ptr::null_mut();

    let base = clockwork_shm_base_checked();
    let Some(g) = geometry() else { return 0 };
    if base.is_null() || index < 0 || index as usize >= g.slots() {
        return 0;
    }
    let index = index as usize;

    // Format the slot. The one authority on channel sanitisation is here and
    // in the writer, and they agree: zero or more than the ring interleaves
    // means "all of them".
    let channels = match u32::try_from(channels).ok() {
        Some(c) if c > 0 && c <= g.channels => c,
        _ => g.channels,
    };
    // SAFETY: the index is inside the geometry the host reported, and the
    // region it describes is mapped for the life of the process.
    let slot = unsafe {
        let slot = slot_ptr(base, &g, index);
        (*slot).channels = channels;
        (*slot).capacity_frames = g.ring_frames;
        (*slot).write_position.store(0, Ordering::Relaxed);
        (*slot).base_engine_frames.store(0, Ordering::Relaxed);
        (*slot).state.store(1, Ordering::Release);
        header_field(base, &g, HDR_ACTIVE_COUNT).fetch_add(1, Ordering::Relaxed);
        header_field(base, &g, HDR_VERSION).fetch_add(1, Ordering::Relaxed);
        slot
    };

    // The handle carries the slot's identity so release can check ownership,
    // and nothing else. `data` stays null: the legacy region-pointer contract
    // has no stream equivalent, and the ugen appends through its own writer.
    h.data = std::ptr::null_mut();
    h.internalData = slot.cast::<c_void>();
    h.channels = 0;
    h.maxFrames = 0;
    SLOT_OWNERS[index].store(hnd as usize, Ordering::Relaxed);
    1
}

/// Nothing to do: scope streams publish on every write.
///
/// The entry exists because the interface table has the slot, and a ugen built
/// against an older engine may still call it.
#[no_mangle]
pub extern "C" fn clockwork_scope_push(_ctx: *mut c_void, _hnd: *mut ScopeBufferHnd, _frames: c_int) {}

/// Give a slot back.
///
/// # Safety
/// `hnd` must point at a live handle.
#[no_mangle]
pub unsafe extern "C" fn clockwork_scope_release(_ctx: *mut c_void, hnd: *mut ScopeBufferHnd) {
    if hnd.is_null() {
        return;
    }
    // SAFETY: a live handle, per the contract.
    let h = unsafe { &mut *hnd };
    if h.internalData.is_null() {
        return;
    }
    let slot = h.internalData.cast::<SlotHeader>();
    let base = clockwork_shm_base_checked();

    // Which slot this is, when the region is still there to say. Integer
    // arithmetic, not pointer arithmetic: a handle can outlive the region it
    // was claimed in, and the point of the check is to find out whether the
    // slot is inside the region that exists now.
    let located = match (base.is_null(), geometry()) {
        (false, Some(g)) => slot_index_of(base, &g, slot).map(|i| (i, g)),
        _ => None,
    };

    // A slot outside the current region is not touched at all: the memory
    // it points into has been unmapped, or belongs to a region this claim
    // knew nothing about. The handle is nulled below either way.
    if let Some((i, g)) = located {
        // Only the current owner may mark it free. A superseded unit's late
        // destructor — the previous run's scope node tearing down under an
        // FX kill-delay tail, after a re-run re-claimed the slot — would
        // otherwise store 0 under the live writer, and readers gate on
        // state == 1: the scope greys out forever while the audio plays on.
        let is_owner = SLOT_OWNERS[i].load(Ordering::Relaxed) == hnd as usize;
        // SAFETY: the slot was just shown to lie inside the mapped region.
        unsafe {
            if is_owner {
                (*slot).state.store(0, Ordering::Relaxed);
                SLOT_OWNERS[i].store(0, Ordering::Relaxed);
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
}

// ── A first-party writer ────────────────────────────────────────────────────
//
// Everything above serves the C++ `ScopeOut2`, which claims a slot through the
// interface table and then appends through its own C++ ring writer. A ugen
// written in Rust has neither, and cannot grow one: the ugen module forbids
// `unsafe`, so it cannot touch shared memory itself.
//
// So the same three operations are offered here as safe functions. The ring
// format is not re-derived — it is the one `shm_scope_stream::write` uses,
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
pub fn stream_activate(index: usize, channels: u32) -> bool {
    let Some(g) = geometry() else { return false };
    let base = clockwork_shm_base_checked();
    if base.is_null() || index >= g.slots() {
        return false;
    }
    // SAFETY: the index is inside the geometry the host reported, and the
    // region it describes is mapped for the life of the process.
    unsafe {
        let slot = slot_ptr(base, &g, index);
        let channels = if channels > 0 && channels <= g.channels { channels } else { g.channels };
        (*slot).channels = channels;
        (*slot).capacity_frames = g.ring_frames;
        (*slot).write_position.store(0, Ordering::Relaxed);
        (*slot).base_engine_frames.store(0, Ordering::Relaxed);
        (*slot).state.store(1, Ordering::Release);
        header_field(base, &g, HDR_ACTIVE_COUNT).fetch_add(1, Ordering::Relaxed);
        header_field(base, &g, HDR_VERSION).fetch_add(1, Ordering::Relaxed);
    }
    true
}

/// Append one block of interleaved audio to slot `index`.
///
/// `engine_frames` is the engine sample position of the first frame. On the
/// first write it anchors the stream; later it heals a discontinuity, so a
/// paused loop resumes at the right place on the display rather than drawing
/// its gap as audio.
pub fn stream_write(index: usize, interleaved: &[f32], frames: usize, engine_frames: u64) {
    if frames == 0 {
        return;
    }
    let Some(g) = geometry() else { return };
    let base = clockwork_shm_base_checked();
    if base.is_null() || index >= g.slots() {
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
pub fn stream_release(index: usize) {
    let Some(g) = geometry() else { return };
    let base = clockwork_shm_base_checked();
    if base.is_null() || index >= g.slots() {
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

/// The shared-memory base, as a byte pointer.
fn clockwork_shm_base_checked() -> *mut u8 {
    // SAFETY: the host defines this; it returns null before the region exists,
    // which every caller above checks for.
    unsafe { clockwork_shm_base().cast::<u8>() }
}

/// The host services, for this crate's own unit-test binary.
///
/// That binary references both symbols and, on its own, defines neither:
/// the hostless archive build.rs makes answers when the `host` feature is
/// off, and when it is on the real definitions are expected from an engine
/// on the link line — which a unit test of this crate never has. Cargo
/// unifies features across one invocation, so `cargo test --workspace`
/// beside a crate that asks for `host` (clockwork-client's tests do,
/// through clockwork-native) builds this binary with `host` on and nothing
/// to resolve the calls: LNK2019 on Windows. These definitions are always
/// in the binary, so it links either way; when the archive is present too
/// the linker takes these first and never opens it.
#[cfg(test)]
mod hostless {
    use core::ffi::{c_int, c_void};

    #[no_mangle]
    pub extern "C" fn clockwork_shm_base() -> *mut c_void {
        core::ptr::null_mut()
    }

    /// # Safety
    /// Nothing is written: the out-pointers are not touched, and 0 says so.
    #[no_mangle]
    pub unsafe extern "C" fn clockwork_scope_geometry(
        _start: *mut usize,
        _header_size: *mut usize,
        _slot_size: *mut usize,
        _max_scopes: *mut usize,
        _ring_frames: *mut u32,
        _channels: *mut u32,
        _track_start: *mut usize,
        _track_slots: *mut usize,
    ) -> c_int {
        0
    }
}
