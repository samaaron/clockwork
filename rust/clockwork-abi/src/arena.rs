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
}

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
    pub reserved: [u32; 5],
    pub entries: [ClockworkArenaEntry; CLOCKWORK_ARENA_MAX_ENTRIES as usize],
}

impl ClockworkArenaHeader {
    /// The entry for a region, if the arena has one.
    pub fn find(&self, id: ClockworkArenaRegion) -> Option<&ClockworkArenaEntry> {
        let n = (self.entry_count as usize).min(CLOCKWORK_ARENA_MAX_ENTRIES as usize);
        self.entries[..n].iter().find(|e| e.id == id.0)
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
        Ok(())
    }
}
