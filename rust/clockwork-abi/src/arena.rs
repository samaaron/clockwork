// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The arena's table of contents (`src/clockwork_arena.h`, `docs/ARENA.md`).
//!
//! The first bytes of every arena: a magic, a version, a table of AREAS and
//! a table of REGIONS — where each is, how big, who writes it, who reads it,
//! how often the engine touches it, the RAM speed it needs, when it is
//! reset, and the geometry a reader walks it by. Looked up BY ID, never by
//! position. A client with only the base pointer finds everything from
//! here; so does a host on the lanes ABI, through
//! [`crate::lanes::clockwork_arena_header`].

pub const CLOCKWORK_ARENA_MAGIC: u32 = 0x4357_4152; // 'CWAR'
pub const CLOCKWORK_ARENA_VERSION: u32 = 2;
pub const CLOCKWORK_ARENA_HEADER_BYTES: u32 = 4096;
pub const CLOCKWORK_ARENA_MAX_AREAS: u32 = 16;
pub const CLOCKWORK_ARENA_MAX_ENTRIES: u32 = 48;
pub const CLOCKWORK_ARENA_GEOM_WORDS: usize = 8;
pub const CLOCKWORK_ARENA_LAYING_OUT: u32 = 0;
pub const CLOCKWORK_ARENA_PUBLISHED: u32 = 1;

/// The twelve areas: five in the arena, four outside it, and the program's own three.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub struct ClockworkArenaArea(pub u32);
impl ClockworkArenaArea {
    /// Public state any client reads: metrics, stats, the clocks, the channel map.
    pub const CLOCKWORK_STATE: Self = Self(1);
    /// Audio streams any client reads: the scope and the taps.
    pub const CLOCKWORK_AUDIO: Self = Self(2);
    /// Messages, through the client code only: the cursors, the counter, the rings.
    pub const CLOCKWORK_TRANSPORT: Self = Self(3);
    /// The guest's window, in the guest's format.
    pub const GUEST_PUBLIC: Self = Self(4);
    /// The guest's config and persistent store.
    pub const GUEST_PRIVATE: Self = Self(5);
    /// Outside the arena: bulk the client hands the guest, read in place.
    pub const ASSETS: Self = Self(6);
    /// Outside the arena: bulk the guest hands the client.
    pub const RETURNS: Self = Self(7);
    /// Outside the arena: the guest's bulk memory (`DspConfig::memory_bulk`), the span clockwork's allocator runs
    /// over for it.
    pub const GUEST_MEMORY_BULK: Self = Self(8);
    /// Outside the arena: the guest's fast memory (`DspConfig::memory`), laid out by the guest by hand.
    pub const GUEST_MEMORY_FAST: Self = Self(9);
    /// The program's static data; the arena lies inside it.
    pub const STATIC_DATA: Self = Self(10);
    /// The engine thread's call stack: on the web, the audio worklet's.
    pub const STACK: Self = Self(11);
    /// What the program's code has taken with malloc and new, to the frontier at boot.
    pub const MALLOC: Self = Self(12);
    /// Outside the arena: what clockwork's Rust code allocates inside an engine entry point (a sink's or a port's
    /// queues); one block for the life of the process, which the RT router serves.
    pub const RUST_HEAP: Self = Self(13);
    /// The engine's memory: one block the guest's two spans are carved from. On the web it holds the arena at its
    /// start and the rust heap too, and lies inside what malloc took; natively the arena and the rust heap are
    /// outside it.
    pub const ENGINE_MEMORY: Self = Self(14);
}

/// Where an area's base is measured from.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub struct ClockworkArenaSpace(pub u32);
impl ClockworkArenaSpace {
    /// Not stated: the engine has not placed it, or this build has none.
    pub const UNKNOWN: Self = Self(0);
    /// An offset from the arena base.
    pub const ARENA: Self = Self(1);
    /// An offset from the shared segment's base.
    pub const SEGMENT: Self = Self(2);
    /// An address in the engine's process (on the web, in wasm memory).
    pub const PROCESS: Self = Self(3);
}

/// Who READS a region: a bit set. Owner says who writes; this says who it
/// is for. 0 is nobody but the owner.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub struct ClockworkArenaAudience(pub u32);
impl ClockworkArenaAudience {
    /// A data contract for any reader in a client process, read by hand.
    pub const PUBLIC: Self = Self(1);
    /// The guest, through a pointer clockwork hands it.
    pub const GUEST: Self = Self(2);
    /// The host.
    pub const HOST: Self = Self(4);
    /// The client code only, never by hand.
    pub const TRANSPORT: Self = Self(8);
}

/// Who writes a region.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkArenaOwner(pub u32);
impl ClockworkArenaOwner {
    pub const CLOCKWORK: Self = Self(1);
    pub const GUEST: Self = Self(2);
    pub const CLIENT: Self = Self(3);
    pub const HOST: Self = Self(4);
}

/// How often the engine thread touches a region.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkArenaCadence(pub u32);
impl ClockworkArenaCadence {
    /// Every block, inside the tick.
    pub const BLOCK: Self = Self(1);
    /// When something changes: a message, a device, a node.
    pub const CHANGE: Self = Self(2);
    /// At boot, then not at all.
    pub const BOOT: Self = Self(3);
}

/// The RAM speed a region needs.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkArenaTier(pub u32);
impl ClockworkArenaTier {
    pub const FAST: Self = Self(1);
    pub const BULK: Self = Self(2);
}

/// When a region's bytes are reset.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkArenaLifetime(pub u32);
impl ClockworkArenaLifetime {
    /// Never: they live as long as the arena.
    pub const ARENA: Self = Self(1);
    /// At a cold boot only; a rebuild keeps them.
    pub const COLD_BOOT: Self = Self(2);
    /// At every build of the engine: a device change too.
    pub const REBUILD: Self = Self(3);
}

/// The regions. Stable numbers, in the order they lie in memory, each named
/// for its half.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkArenaRegion(pub u32);
impl ClockworkArenaRegion {
    // clockwork-state
    pub const CLOCKWORK_METRICS: Self = Self(1);
    pub const CLOCKWORK_NATIVE_STATS: Self = Self(2);
    pub const CLOCKWORK_CLOCK_ANCHORS: Self = Self(3);
    pub const CLOCKWORK_CLOCK_STATE: Self = Self(4);
    pub const CLOCKWORK_SAMPLE_CLOCK: Self = Self(5);
    pub const CLOCKWORK_CHANNEL_MAP: Self = Self(6);
    // clockwork-audio
    pub const CLOCKWORK_SCOPE: Self = Self(7);
    /// Tap streams: a header, the guest's slots, then the engine's track
    /// slots, one index space (`geom::TAP_*`).
    pub const CLOCKWORK_TAPS: Self = Self(8);
    // clockwork-transport
    pub const CLOCKWORK_RING_CURSORS: Self = Self(9);
    pub const CLOCKWORK_NODE_ID_COUNTER: Self = Self(10);
    pub const CLOCKWORK_IN_RING: Self = Self(11);
    pub const CLOCKWORK_RT_OUT_RING: Self = Self(12);
    pub const CLOCKWORK_NRT_OUT_RING: Self = Self(13);
    /// Clockwork's own control verbs, written at the boundary and drained by
    /// the control pass with no audio block in the loop (docs/RESTARTS.md).
    /// Numbered after the guest's regions, laid out in the transport area.
    pub const CLOCKWORK_CONTROL_RING: Self = Self(17);
    // guest-public
    pub const GUEST_WINDOW: Self = Self(14);
    // guest-private
    pub const GUEST_CONFIG: Self = Self(15);
    pub const GUEST_PERSIST: Self = Self(16);
}

/// The two scope slots, as slot indices into the scope: what left for the
/// device this block, and what arrived from it. Written by clockwork inside
/// every tick, on every host.
pub const CLOCKWORK_TAP_OUT: u32 = 0;
pub const CLOCKWORK_TAP_IN: u32 = 1;

/// Geometry word indices, per region (`ClockworkArenaGeom`).
pub mod geom {
    pub const RING_MAX_MESSAGE: usize = 0;
    pub const RING_MESSAGE_MAGIC: usize = 1;
    pub const RING_PADDING_MAGIC: usize = 2;
    pub const RING_PADDING_MARKER: usize = 3;
    pub const RING_HEADER_BYTES: usize = 4;
    pub const METRICS_FIELDS: usize = 0;
    pub const ANCHOR_GLOBAL: usize = 0;
    pub const SCOPE_SLOTS: usize = 0;
    pub const SCOPE_SLOT_BYTES: usize = 1;
    pub const SCOPE_HEADER_BYTES: usize = 2;
    pub const SCOPE_FRAMES: usize = 3;
    pub const SCOPE_CHANNELS: usize = 4;
    pub const SCOPE_SAMPLE_RATE: usize = 5;
    /// Taps: a header, the guest's `TAP_SLOTS` slots, then `TAP_TRACK_SLOTS`
    /// track slots, every slot the same shape, numbered from 0.
    pub const TAP_SLOTS: usize = 0;
    pub const TAP_HEADER_BYTES: usize = 1;
    pub const TAP_SLOT_BYTES: usize = 2;
    pub const TAP_SLOT_HEADER: usize = 3;
    pub const TAP_RING_FRAMES: usize = 4;
    pub const TAP_CHANNELS: usize = 5;
    pub const TAP_TRACK_SLOTS: usize = 6;
    pub const TAP_TRACK_FIRST: usize = 7;
    /// The guest window: what the guest declared its window to be
    /// (`DspInfo::window_magic`, `window_version`), 0 until it binds.
    pub const WINDOW_MAGIC: usize = 0;
    pub const WINDOW_VERSION: usize = 1;
}

// No region's geometry reaches past the words an entry holds.
const _: () = assert!(geom::TAP_TRACK_FIRST < CLOCKWORK_ARENA_GEOM_WORDS);
const _: () = assert!(geom::SCOPE_SAMPLE_RATE < CLOCKWORK_ARENA_GEOM_WORDS);

/// One area. 24 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkArenaAreaEntry {
    pub area: u32,
    pub space: u32,
    pub base_lo: u32,
    pub base_hi: u32,
    pub bytes_lo: u32,
    pub bytes_hi: u32,
}

impl ClockworkArenaAreaEntry {
    /// The offset or address, as one number.
    pub fn base(&self) -> u64 {
        self.base_lo as u64 | (self.base_hi as u64) << 32
    }
    pub fn bytes(&self) -> u64 {
        self.bytes_lo as u64 | (self.bytes_hi as u64) << 32
    }
}

/// One region. 64 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkArenaEntry {
    pub id: u32,
    pub offset: u32,
    pub bytes: u32,
    pub owner: u32,
    pub audience: u32,
    pub cadence: u32,
    pub tier: u32,
    pub lifetime: u32,
    pub geom: [u32; CLOCKWORK_ARENA_GEOM_WORDS],
}

impl ClockworkArenaEntry {
    /// A data contract a client may read by hand.
    pub fn public(&self) -> bool {
        self.audience & ClockworkArenaAudience::PUBLIC.0 != 0
    }
}

/// The header, at arena offset 0.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkArenaHeader {
    pub magic: u32,
    pub version: u32,
    pub header_bytes: u32,
    pub instance_id: u32,
    pub arena_bytes: u32,
    /// Clockwork's half: `[0, clockwork_bytes)`.
    pub clockwork_bytes: u32,
    /// The guest's half: `[guest_offset, guest_offset + guest_bytes)`.
    pub guest_offset: u32,
    pub guest_bytes: u32,
    pub area_count: u32,
    pub area_bytes: u32,
    pub entry_count: u32,
    pub entry_bytes: u32,
    pub state: u32,
    /// The guest instance's generation, as one word (docs/RESTARTS.md):
    /// `2g+1` while there is no instance, `2g` while there is one, `g` the
    /// generation the last build opened — the number `/clockwork/setup`
    /// names. 1 in a new arena. Read it with one load, and ask again: it
    /// moves while a reader holds the header.
    pub generation: u32,
    pub reserved: [u32; 2],
    pub areas: [ClockworkArenaAreaEntry; CLOCKWORK_ARENA_MAX_AREAS as usize],
    pub entries: [ClockworkArenaEntry; CLOCKWORK_ARENA_MAX_ENTRIES as usize],
}

const PUBLIC_AREAS: [u32; 3] = [
    ClockworkArenaArea::CLOCKWORK_STATE.0,
    ClockworkArenaArea::CLOCKWORK_AUDIO.0,
    ClockworkArenaArea::GUEST_PUBLIC.0,
];

impl ClockworkArenaHeader {
    /// The generation the word names: the last one opened, 0 before any.
    pub fn instance_generation(&self) -> u32 { self.generation >> 1 }

    /// Whether a guest instance is up, from the word.
    pub fn has_instance(&self) -> bool { self.generation & 1 == 0 }

    /// The entry for a region, if the arena has one.
    pub fn find(&self, id: ClockworkArenaRegion) -> Option<&ClockworkArenaEntry> {
        let n = (self.entry_count as usize).min(CLOCKWORK_ARENA_MAX_ENTRIES as usize);
        self.entries[..n].iter().find(|e| e.id == id.0)
    }

    /// The area entry for an area, if the table has one.
    pub fn area(&self, area: ClockworkArenaArea) -> Option<&ClockworkArenaAreaEntry> {
        let n = (self.area_count as usize).min(CLOCKWORK_ARENA_MAX_AREAS as usize);
        self.areas[..n].iter().find(|a| a.area == area.0)
    }

    /// The area a region lies in, by its offset; `None` when it lies in none.
    pub fn area_of(&self, e: &ClockworkArenaEntry) -> Option<ClockworkArenaArea> {
        let n = (self.area_count as usize).min(CLOCKWORK_ARENA_MAX_AREAS as usize);
        self.areas[..n]
            .iter()
            .filter(|a| a.space == ClockworkArenaSpace::ARENA.0)
            .find(|a| e.offset as u64 >= a.base() && (e.offset + e.bytes) as u64 <= a.base() + a.bytes())
            .map(|a| ClockworkArenaArea(a.area))
    }

    /// True when the arena has region `id` and it is a data contract a
    /// client may read by hand. False for a region the arena lacks, and one
    /// that is transport or private.
    pub fn public(&self, id: ClockworkArenaRegion) -> bool {
        self.find(id).is_some_and(|e| e.public())
    }

    /// True when this is a published table of a version this crate knows,
    /// fitting in `bytes`, whose areas and regions agree with themselves.
    /// `Err` names the first failure, in the C header's words.
    pub fn check(&self, bytes: u32) -> Result<(), &'static str> {
        if bytes < core::mem::size_of::<Self>() as u32 { return Err("no room for an arena header"); }
        if self.magic != CLOCKWORK_ARENA_MAGIC { return Err("not a clockwork arena"); }
        if self.version != CLOCKWORK_ARENA_VERSION { return Err("arena version this reader does not know"); }
        if self.state != CLOCKWORK_ARENA_PUBLISHED { return Err("arena not yet published"); }
        if self.header_bytes < core::mem::size_of::<Self>() as u32 { return Err("header reservation smaller than the header"); }
        if self.area_bytes != core::mem::size_of::<ClockworkArenaAreaEntry>() as u32 { return Err("area shape this reader does not know"); }
        if self.entry_bytes != core::mem::size_of::<ClockworkArenaEntry>() as u32 { return Err("entry shape this reader does not know"); }
        if self.area_count > CLOCKWORK_ARENA_MAX_AREAS { return Err("more areas than the table holds"); }
        if self.entry_count > CLOCKWORK_ARENA_MAX_ENTRIES { return Err("more entries than the table holds"); }
        if self.arena_bytes > bytes { return Err("arena larger than the memory it is in"); }
        if self.clockwork_bytes > self.arena_bytes { return Err("clockwork's half larger than the arena"); }
        if self.guest_offset != self.clockwork_bytes { return Err("the guest's half does not start where clockwork's ends"); }
        if self.guest_bytes != self.arena_bytes - self.guest_offset { return Err("the two halves do not cover the arena"); }
        let areas = &self.areas[..self.area_count as usize];
        for (i, a) in areas.iter().enumerate() {
            if a.area == 0 { return Err("an empty area inside the count"); }
            if areas[..i].iter().any(|b| b.area == a.area) { return Err("an area listed twice"); }
            if a.space == ClockworkArenaSpace::ARENA.0 {
                let end = a.base_lo as u64 + a.bytes_lo as u64;
                let guests = a.area == ClockworkArenaArea::GUEST_PUBLIC.0 || a.area == ClockworkArenaArea::GUEST_PRIVATE.0;
                if a.base_hi != 0 || a.bytes_hi != 0 { return Err("an area in the arena with a 64-bit base"); }
                if a.base_lo < self.header_bytes || end > self.arena_bytes as u64 { return Err("an area outside the arena"); }
                if if guests { a.base_lo < self.guest_offset } else { end > self.clockwork_bytes as u64 } {
                    return Err("an area in the wrong half");
                }
                for b in areas[..i].iter().filter(|b| b.space == ClockworkArenaSpace::ARENA.0) {
                    if (a.base_lo as u64) < b.base_lo as u64 + b.bytes_lo as u64 && (b.base_lo as u64) < end {
                        return Err("two areas overlap");
                    }
                }
            } else if a.area <= ClockworkArenaArea::GUEST_PRIVATE.0 {
                return Err("an area of the arena placed outside it");
            }
        }
        for e in &self.entries[..self.entry_count as usize] {
            if e.id == 0 { return Err("an empty entry inside the count"); }
            if e.offset < self.header_bytes { return Err("a region overlaps the header"); }
            if e.offset > self.arena_bytes || e.bytes > self.arena_bytes - e.offset {
                return Err("a region runs past the arena");
            }
            if !(1..=4).contains(&e.owner) { return Err("a region with no owner"); }
            if !(1..=15).contains(&e.audience) { return Err("a region with no audience"); }
            if !(1..=3).contains(&e.cadence) { return Err("a region with no cadence"); }
            if !(1..=2).contains(&e.tier) { return Err("a region with no tier"); }
            if !(1..=3).contains(&e.lifetime) { return Err("a region with no lifetime"); }
            let Some(area) = self.area_of(e) else { return Err("a region in no area") };
            let public_area = PUBLIC_AREAS.contains(&area.0);
            if public_area && !e.public() { return Err("a non-public region in a public area"); }
            if !public_area && e.public() { return Err("a public region outside the public areas"); }
            if area == ClockworkArenaArea::CLOCKWORK_TRANSPORT && e.audience & ClockworkArenaAudience::TRANSPORT.0 == 0 {
                return Err("a region in the transport area that is not transport");
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(id: u32, offset: u32, bytes: u32, audience: u32) -> ClockworkArenaEntry {
        ClockworkArenaEntry {
            id, offset, bytes,
            owner: ClockworkArenaOwner::CLOCKWORK.0, audience,
            cadence: ClockworkArenaCadence::BLOCK.0, tier: ClockworkArenaTier::FAST.0,
            lifetime: ClockworkArenaLifetime::REBUILD.0,
            geom: [0; CLOCKWORK_ARENA_GEOM_WORDS],
        }
    }
    fn area(area: u32, base: u32, bytes: u32) -> ClockworkArenaAreaEntry {
        ClockworkArenaAreaEntry { area, space: ClockworkArenaSpace::ARENA.0, base_lo: base, base_hi: 0, bytes_lo: bytes, bytes_hi: 0 }
    }
    const NONE: ClockworkArenaAreaEntry = ClockworkArenaAreaEntry { area: 0, space: 0, base_lo: 0, base_hi: 0, bytes_lo: 0, bytes_hi: 0 };

    // A small arena: a state area of two regions, a transport area of one;
    // a guest half with a public window and a private store.
    fn header() -> ClockworkArenaHeader {
        let mut h = ClockworkArenaHeader {
            magic: CLOCKWORK_ARENA_MAGIC, version: CLOCKWORK_ARENA_VERSION,
            header_bytes: CLOCKWORK_ARENA_HEADER_BYTES, instance_id: 0,
            arena_bytes: 12288, clockwork_bytes: 8192, guest_offset: 8192, guest_bytes: 4096,
            area_count: 4, area_bytes: core::mem::size_of::<ClockworkArenaAreaEntry>() as u32,
            entry_count: 5, entry_bytes: core::mem::size_of::<ClockworkArenaEntry>() as u32,
            state: CLOCKWORK_ARENA_PUBLISHED, generation: 1, reserved: [0; 2],
            areas: [NONE; CLOCKWORK_ARENA_MAX_AREAS as usize],
            entries: [entry(0, 0, 0, 0); CLOCKWORK_ARENA_MAX_ENTRIES as usize],
        };
        h.areas[0] = area(ClockworkArenaArea::CLOCKWORK_STATE.0, 4096, 2048);
        h.areas[1] = area(ClockworkArenaArea::CLOCKWORK_TRANSPORT.0, 6144, 2048);
        h.areas[2] = area(ClockworkArenaArea::GUEST_PUBLIC.0, 8192, 2048);
        h.areas[3] = area(ClockworkArenaArea::GUEST_PRIVATE.0, 10240, 2048);
        h.entries[0] = entry(ClockworkArenaRegion::CLOCKWORK_METRICS.0, 4096, 1024, ClockworkArenaAudience::PUBLIC.0);
        h.entries[1] = entry(ClockworkArenaRegion::CLOCKWORK_CLOCK_STATE.0, 5120, 1024,
                             ClockworkArenaAudience::PUBLIC.0 | ClockworkArenaAudience::GUEST.0);
        h.entries[2] = entry(ClockworkArenaRegion::CLOCKWORK_IN_RING.0, 6144, 2048, ClockworkArenaAudience::TRANSPORT.0);
        h.entries[3] = entry(ClockworkArenaRegion::GUEST_WINDOW.0, 8192, 2048, ClockworkArenaAudience::PUBLIC.0);
        h.entries[4] = entry(ClockworkArenaRegion::GUEST_PERSIST.0, 10240, 2048, ClockworkArenaAudience::GUEST.0);
        h
    }

    #[test]
    fn a_well_laid_out_table_passes_and_says_what_is_public() {
        let h = header();
        assert_eq!(h.check(12288), Ok(()));
        assert!(h.public(ClockworkArenaRegion::CLOCKWORK_METRICS));
        assert!(h.public(ClockworkArenaRegion::GUEST_WINDOW));
        assert!(!h.public(ClockworkArenaRegion::CLOCKWORK_IN_RING));
        assert!(!h.public(ClockworkArenaRegion::GUEST_PERSIST));
        assert!(!h.public(ClockworkArenaRegion::CLOCKWORK_TAPS)); // absent
        assert_eq!(h.find(ClockworkArenaRegion::CLOCKWORK_CLOCK_STATE).unwrap().audience & ClockworkArenaAudience::GUEST.0, 2);
        assert_eq!(h.area_of(&h.entries[2]), Some(ClockworkArenaArea::CLOCKWORK_TRANSPORT));
        assert!(h.area(ClockworkArenaArea::GUEST_MEMORY_BULK).is_none());
    }

    #[test]
    fn a_public_region_in_the_transport_area_is_refused() {
        let mut h = header();
        h.entries[2].audience = ClockworkArenaAudience::PUBLIC.0;
        assert_eq!(h.check(12288), Err("a public region outside the public areas"));
    }

    #[test]
    fn a_non_public_region_in_a_public_area_is_refused() {
        let mut h = header();
        h.entries[0].audience = ClockworkArenaAudience::TRANSPORT.0;
        assert_eq!(h.check(12288), Err("a non-public region in a public area"));
    }

    #[test]
    fn an_area_in_the_wrong_half_or_overlapping_is_refused() {
        let mut h = header();
        h.areas[1].bytes_lo = 4096;   // transport runs into the guest's half
        assert_eq!(h.check(12288), Err("an area in the wrong half"));
        let mut h = header();
        h.areas[1].base_lo = 5120;    // transport overlaps state
        assert_eq!(h.check(12288), Err("two areas overlap"));
    }

    #[test]
    fn a_region_outside_every_area_is_refused() {
        let mut h = header();
        h.entries[0].offset = 4096 + 1536;   // runs out of the state area
        assert_eq!(h.check(12288), Err("a region in no area"));
    }

    #[test]
    fn a_region_with_a_fact_missing_is_refused() {
        let mut h = header();
        h.entries[1].tier = 0;
        assert_eq!(h.check(12288), Err("a region with no tier"));
    }

    #[test]
    fn the_generation_word_says_whether_there_is_an_instance_and_which_one() {
        // One word, one load (docs/RESTARTS.md): odd between instances, even
        // with one; the rest is the generation the last build opened, the
        // number /clockwork/setup named. A new arena is at 1: none yet.
        let mut h = header();
        assert!(!h.has_instance());
        assert_eq!(h.instance_generation(), 0);
        h.generation = 2;
        assert!(h.has_instance());
        assert_eq!(h.instance_generation(), 1);
        h.generation = 3;
        assert!(!h.has_instance());
        assert_eq!(h.instance_generation(), 1);   // the one closing is still the one named
        h.generation = 4;
        assert!(h.has_instance());
        assert_eq!(h.instance_generation(), 2);
        // The word took the first reserved slot: nothing after it moved.
        assert_eq!(core::mem::offset_of!(ClockworkArenaHeader, generation),
                   core::mem::offset_of!(ClockworkArenaHeader, state) + 4);
        assert_eq!(core::mem::offset_of!(ClockworkArenaHeader, areas),
                   core::mem::offset_of!(ClockworkArenaHeader, generation) + 12);
    }
}
