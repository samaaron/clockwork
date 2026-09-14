// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The arena's table of contents (`src/clockwork_arena.h`, `docs/ARENA.md`).
//!
//! The first bytes of every arena: a magic, a version, and a table naming
//! each region — where, how big, who writes it, and the geometry a reader
//! walks it by. Looked up BY ID, never by position. A client with only the
//! base pointer finds everything from here; so does a host on the lanes ABI,
//! through [`crate::lanes::clockwork_arena_header`].

pub const CLOCKWORK_ARENA_MAGIC: u32 = 0x4357_4152; // 'CWAR'
pub const CLOCKWORK_ARENA_VERSION: u32 = 1;
pub const CLOCKWORK_ARENA_HEADER_BYTES: u32 = 4096;
pub const CLOCKWORK_ARENA_MAX_ENTRIES: u32 = 48;
pub const CLOCKWORK_ARENA_GEOM_WORDS: usize = 12;
pub const CLOCKWORK_ARENA_LAYING_OUT: u32 = 0;
pub const CLOCKWORK_ARENA_PUBLISHED: u32 = 1;

/// Who READS a region: a bit set in the last geometry word of every entry
/// (`geom::AUDIENCE`). Owner says who writes; this says who it is for.
/// 0 is nobody but the owner — or a writer from before the word existed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(transparent)]
pub struct ClockworkArenaAudience(pub u32);
impl ClockworkArenaAudience {
    /// A data contract for any reader in a client process, read by hand.
    pub const PUBLISHED: Self = Self(1);
    /// The guest, through a pointer clockwork hands it.
    pub const GUEST: Self = Self(2);
    /// The host.
    pub const HOST: Self = Self(4);
    /// The command plane: the client ABI only, never by hand.
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

/// The regions. Stable numbers.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ClockworkArenaRegion(pub u32);
impl ClockworkArenaRegion {
    pub const CONTROL: Self = Self(1);
    pub const METRICS: Self = Self(2);
    pub const NATIVE_STATS: Self = Self(3);
    pub const CLOCK_STATE: Self = Self(4);
    pub const CLOCK_ANCHORS: Self = Self(5);
    pub const SAMPLE_CLOCK: Self = Self(6);
    pub const CHANNEL_MAP: Self = Self(7);
    pub const NODE_ID_COUNTER: Self = Self(8);
    pub const IN_RING: Self = Self(9);
    pub const OUT_RING: Self = Self(10);
    pub const NRT_OUT_RING: Self = Self(11);
    pub const AUDIO_TAPS: Self = Self(12);
    pub const CLIENT_SLOTS: Self = Self(13);
    pub const GUEST_CONFIG: Self = Self(14);
    pub const GUEST_WINDOW: Self = Self(15);
    pub const SCOPE: Self = Self(16);
    pub const GUEST_PERSIST: Self = Self(17);
    /// Scope-stream slots the engine writes from plugin-track returns,
    /// numbered after the guest's scope slots in one index space.
    pub const TRACK_TAPS: Self = Self(18);
}

/// The two audio taps, as slot indices into `AUDIO_TAPS`: what left for the
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
    pub const ANCHOR_NTP_START: usize = 0;
    pub const ANCHOR_DRIFT: usize = 1;
    pub const ANCHOR_GLOBAL: usize = 2;
    pub const TAPS_SLOTS: usize = 0;
    pub const TAPS_SLOT_BYTES: usize = 1;
    pub const TAPS_HEADER_BYTES: usize = 2;
    pub const TAPS_FRAMES: usize = 3;
    pub const TAPS_CHANNELS: usize = 4;
    pub const TAPS_SAMPLE_RATE: usize = 5;
    pub const SCOPE_SLOTS: usize = 0;
    pub const SCOPE_HEADER_BYTES: usize = 1;
    pub const SCOPE_SLOT_BYTES: usize = 2;
    pub const SCOPE_SLOT_HEADER: usize = 3;
    pub const SCOPE_RING_FRAMES: usize = 4;
    pub const SCOPE_CHANNELS: usize = 5;
    pub const TRACK_SLOTS: usize = 0;
    pub const TRACK_SLOT_BYTES: usize = 1;
    pub const TRACK_SLOT_HEADER: usize = 2;
    pub const TRACK_RING_FRAMES: usize = 3;
    pub const TRACK_CHANNELS: usize = 4;
    pub const TRACK_FIRST_INDEX: usize = 5;
    pub const SLOTS_COUNT: usize = 0;
    pub const SLOTS_SLOT_BYTES: usize = 1;
    pub const SLOTS_HEADER_BYTES: usize = 2;
    pub const SLOTS_STRUCTS_OFF: usize = 3;
    pub const SLOTS_STRUCTS_BYTES: usize = 4;
    pub const SLOTS_STACK_OFF: usize = 5;
    pub const SLOTS_STACK_BYTES: usize = 6;
    /// EVERY region: the audience (`ClockworkArenaAudience` bits), in the
    /// last word. A region's own geometry stops short of it.
    pub const AUDIENCE: usize = super::CLOCKWORK_ARENA_GEOM_WORDS - 1;
}

// The audience word is the last geometry word, and no region's own geometry
// reaches it. Pinned at compile time, where the C header pins the same.
const _: () = assert!(geom::AUDIENCE == CLOCKWORK_ARENA_GEOM_WORDS - 1);
const _: () = assert!(geom::SLOTS_STACK_BYTES < geom::AUDIENCE);
const _: () = assert!(geom::TRACK_FIRST_INDEX < geom::AUDIENCE);

/// One region. 64 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkArenaEntry {
    pub id: u32,
    pub offset: u32,
    pub bytes: u32,
    pub owner: u32,
    pub geom: [u32; CLOCKWORK_ARENA_GEOM_WORDS],
}

impl ClockworkArenaEntry {
    /// Who this region is for (`ClockworkArenaAudience` bits); 0 from a
    /// writer that did not say.
    pub fn audience(&self) -> u32 {
        self.geom[geom::AUDIENCE]
    }
    /// A data contract a client may read by hand.
    pub fn published(&self) -> bool {
        self.audience() & ClockworkArenaAudience::PUBLISHED.0 != 0
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
    pub block_bytes: u32,
    pub guest_offset: u32,
    pub guest_bytes: u32,
    pub entry_count: u32,
    pub entry_bytes: u32,
    pub state: u32,
    /// The block's published run is `[header_bytes, block_published_end)`;
    /// the rest of the block is transport. 0: not stated by the writer.
    pub block_published_end: u32,
    /// The guest's published run is `[guest_offset, guest_published_end)`;
    /// the rest is the guest's own. 0: not stated.
    pub guest_published_end: u32,
    pub reserved: [u32; 3],
    pub entries: [ClockworkArenaEntry; CLOCKWORK_ARENA_MAX_ENTRIES as usize],
}

impl ClockworkArenaHeader {
    /// The entry for a region, if the arena has one.
    pub fn find(&self, id: ClockworkArenaRegion) -> Option<&ClockworkArenaEntry> {
        let n = (self.entry_count as usize).min(CLOCKWORK_ARENA_MAX_ENTRIES as usize);
        self.entries[..n].iter().find(|e| e.id == id.0)
    }

    /// True when the arena has region `id` and it is a data contract a
    /// client may read by hand. False for a region the arena lacks, one
    /// that is transport or private, and one from a writer that never said.
    pub fn published(&self, id: ClockworkArenaRegion) -> bool {
        self.find(id).is_some_and(|e| e.published())
    }

    /// True when this is a published table of a version this crate knows,
    /// fitting in `bytes`, with every region inside the arena it describes.
    /// `Err` names the first failure, in the C header's words.
    pub fn check(&self, bytes: u32) -> Result<(), &'static str> {
        if bytes < core::mem::size_of::<Self>() as u32 { return Err("no room for an arena header"); }
        if self.magic != CLOCKWORK_ARENA_MAGIC { return Err("not a clockwork arena"); }
        if self.version != CLOCKWORK_ARENA_VERSION { return Err("arena version this reader does not know"); }
        if self.state != CLOCKWORK_ARENA_PUBLISHED { return Err("arena not yet published"); }
        if self.header_bytes < core::mem::size_of::<Self>() as u32 { return Err("header reservation smaller than the header"); }
        if self.entry_bytes != core::mem::size_of::<ClockworkArenaEntry>() as u32 { return Err("entry shape this reader does not know"); }
        if self.entry_count > CLOCKWORK_ARENA_MAX_ENTRIES { return Err("more entries than the table holds"); }
        if self.arena_bytes > bytes { return Err("arena larger than the memory it is in"); }
        if self.block_bytes > self.arena_bytes { return Err("clockwork block larger than the arena"); }
        if self.guest_offset > self.arena_bytes || self.guest_bytes > self.arena_bytes - self.guest_offset {
            return Err("guest region outside the arena");
        }
        for e in &self.entries[..self.entry_count as usize] {
            if e.id == 0 { return Err("an empty entry inside the count"); }
            if e.offset < self.header_bytes { return Err("a region overlaps the header"); }
            if e.offset > self.arena_bytes || e.bytes > self.arena_bytes - e.offset {
                return Err("a region runs past the arena");
            }
        }
        // The published runs, when the writer stated them: each inside its
        // half, every published region inside its run, everything else after.
        if self.block_published_end != 0 || self.guest_published_end != 0 {
            if self.block_published_end < self.header_bytes || self.block_published_end > self.block_bytes {
                return Err("the block's published run runs past the block");
            }
            if self.guest_published_end < self.guest_offset
                || self.guest_published_end > self.guest_offset + self.guest_bytes
            {
                return Err("the guest's published run is outside the guest region");
            }
            for e in &self.entries[..self.entry_count as usize] {
                let end = if e.offset >= self.guest_offset { self.guest_published_end } else { self.block_published_end };
                if e.published() && e.offset + e.bytes > end {
                    return Err("a published region outside the published run");
                }
                if !e.published() && e.offset < end {
                    return Err("an unpublished region inside the published run");
                }
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(id: u32, offset: u32, bytes: u32, audience: u32) -> ClockworkArenaEntry {
        let mut geom = [0u32; CLOCKWORK_ARENA_GEOM_WORDS];
        geom[geom::AUDIENCE] = audience;
        ClockworkArenaEntry { id, offset, bytes, owner: ClockworkArenaOwner::CLOCKWORK.0, geom }
    }

    // A small arena: header, a published run of two regions, a transport run
    // of one; a guest half with one published and one private region.
    fn header() -> ClockworkArenaHeader {
        let mut h = ClockworkArenaHeader {
            magic: CLOCKWORK_ARENA_MAGIC, version: CLOCKWORK_ARENA_VERSION,
            header_bytes: CLOCKWORK_ARENA_HEADER_BYTES, instance_id: 0,
            arena_bytes: 12288, block_bytes: 8192, guest_offset: 8192, guest_bytes: 4096,
            entry_count: 5, entry_bytes: core::mem::size_of::<ClockworkArenaEntry>() as u32,
            state: CLOCKWORK_ARENA_PUBLISHED,
            block_published_end: 6144, guest_published_end: 10240, reserved: [0; 3],
            entries: [entry(0, 0, 0, 0); CLOCKWORK_ARENA_MAX_ENTRIES as usize],
        };
        h.entries[0] = entry(ClockworkArenaRegion::METRICS.0, 4096, 1024, ClockworkArenaAudience::PUBLISHED.0);
        h.entries[1] = entry(ClockworkArenaRegion::CLOCK_STATE.0, 5120, 1024,
                             ClockworkArenaAudience::PUBLISHED.0 | ClockworkArenaAudience::GUEST.0);
        h.entries[2] = entry(ClockworkArenaRegion::IN_RING.0, 6144, 2048, ClockworkArenaAudience::TRANSPORT.0);
        h.entries[3] = entry(ClockworkArenaRegion::GUEST_WINDOW.0, 8192, 2048, ClockworkArenaAudience::PUBLISHED.0);
        h.entries[4] = entry(ClockworkArenaRegion::GUEST_PERSIST.0, 10240, 2048, ClockworkArenaAudience::GUEST.0);
        h
    }

    #[test]
    fn a_well_laid_out_table_passes_and_says_who_is_published() {
        let h = header();
        assert_eq!(h.check(12288), Ok(()));
        assert!(h.published(ClockworkArenaRegion::METRICS));
        assert!(h.published(ClockworkArenaRegion::GUEST_WINDOW));
        assert!(!h.published(ClockworkArenaRegion::IN_RING));
        assert!(!h.published(ClockworkArenaRegion::GUEST_PERSIST));
        assert!(!h.published(ClockworkArenaRegion::SCOPE)); // absent
        assert_eq!(h.find(ClockworkArenaRegion::CLOCK_STATE).unwrap().audience() & ClockworkArenaAudience::GUEST.0, 2);
    }

    #[test]
    fn a_published_region_past_its_run_is_refused() {
        let mut h = header();
        h.entries[2].geom[geom::AUDIENCE] = ClockworkArenaAudience::PUBLISHED.0;
        assert_eq!(h.check(12288), Err("a published region outside the published run"));
    }

    #[test]
    fn an_unpublished_region_inside_the_run_is_refused() {
        let mut h = header();
        h.entries[0].geom[geom::AUDIENCE] = ClockworkArenaAudience::TRANSPORT.0;
        assert_eq!(h.check(12288), Err("an unpublished region inside the published run"));
    }

    #[test]
    fn a_boundary_outside_its_half_is_refused() {
        let mut h = header();
        h.block_published_end = 8192 + 16;
        assert_eq!(h.check(12288), Err("the block's published run runs past the block"));
        let mut h = header();
        h.guest_published_end = 8192 - 16;
        assert_eq!(h.check(12288), Err("the guest's published run is outside the guest region"));
    }

    #[test]
    fn a_writer_that_said_nothing_is_not_judged() {
        let mut h = header();
        h.block_published_end = 0;
        h.guest_published_end = 0;
        for e in h.entries.iter_mut() { e.geom[geom::AUDIENCE] = 0; }
        assert_eq!(h.check(12288), Ok(()));
        assert!(!h.published(ClockworkArenaRegion::METRICS));
    }

}
