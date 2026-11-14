// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The parameter table (`src/params/clockwork_params.h`, docs/PARAMS.md):
//! the region's header and one entry, as a reader or a definer lays them
//! out. `tests/layout.rs` holds these to the C header.

use core::ffi::{c_char, c_int};

use crate::lanes::ClockworkCore;

pub const CLOCKWORK_PARAMS_MAGIC: u32 = 0x4357_504D; // 'CWPM'
pub const CLOCKWORK_PARAMS_VERSION: u32 = 2;
pub const CLOCKWORK_PARAM_NAME_BYTES: usize = 32;
pub const CLOCKWORK_PARAM_FULL_NAME_BYTES: usize = 256;
pub const CLOCKWORK_PARAM_GROUP_DEPTH: u32 = 8;

pub const CLOCKWORK_PARAM_RETIRE: u32 = 0;
pub const CLOCKWORK_PARAM_CONTINUOUS: u32 = 1;
pub const CLOCKWORK_PARAM_STEPPED: u32 = 2;
pub const CLOCKWORK_PARAM_SWITCH: u32 = 3;
pub const CLOCKWORK_PARAM_NOTE: u32 = 4;
pub const CLOCKWORK_PARAM_CHOICE: u32 = 5;
pub const CLOCKWORK_PARAM_GROUP: u32 = 6;

pub const CLOCKWORK_PARAM_UNIT_NONE: u32 = 0;
pub const CLOCKWORK_PARAM_UNIT_PERCENT: u32 = 1;
pub const CLOCKWORK_PARAM_UNIT_DECIBELS: u32 = 2;
pub const CLOCKWORK_PARAM_UNIT_HERTZ: u32 = 3;
pub const CLOCKWORK_PARAM_UNIT_SECONDS: u32 = 4;
pub const CLOCKWORK_PARAM_UNIT_BPM: u32 = 5;
pub const CLOCKWORK_PARAM_UNIT_SEMITONES: u32 = 6;
pub const CLOCKWORK_PARAM_UNIT_CUSTOM: u32 = 7;

pub const CLOCKWORK_PARAM_AUTOMATABLE: u32 = 1 << 0;
pub const CLOCKWORK_PARAM_HIDDEN: u32 = 1 << 1;
pub const CLOCKWORK_PARAM_READ_ONLY: u32 = 1 << 2;
pub const CLOCKWORK_PARAM_WRAPS: u32 = 1 << 3;
pub const CLOCKWORK_PARAM_META: u32 = 1 << 4;

pub const CLOCKWORK_PARAM_ROLE_NONE: u32 = 0;
pub const CLOCKWORK_PARAM_ROLE_BYPASS: u32 = 1;
pub const CLOCKWORK_PARAM_ROLE_TEMPO: u32 = 2;
pub const CLOCKWORK_PARAM_ROLE_KEY_NOTE: u32 = 3;
pub const CLOCKWORK_PARAM_ROLE_KEY_HELD: u32 = 4;

pub const CLOCKWORK_PARAM_TAPER_LINEAR: f32 = 0.0;
pub const CLOCKWORK_PARAM_TAPER_LOG: f32 = -1.0;

pub const CLOCKWORK_PARAM_TEXT_FULL_NAME: u8 = 1;
pub const CLOCKWORK_PARAM_TEXT_DOC: u8 = 2;
pub const CLOCKWORK_PARAM_TEXT_UNIT: u8 = 3;
pub const CLOCKWORK_PARAM_TEXT_VALUE: u8 = 4;

pub const CLOCKWORK_PARAM_DEFINER_GUEST: u32 = 0xFFFF_FFFF;
pub const CLOCKWORK_PARAM_DEFINER_HOST: u32 = 0xFFFF_FFFE;

/// One entry: the facts of a parameter, and whether its slot is taken.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkParamEntry {
    pub live: u32,
    pub id: u32,
    pub kind: u32,
    pub unit: u32,
    pub flags: u32,
    pub group: u32,
    pub min: f32,
    pub max: f32,
    pub def: f32,
    pub definer: u32,
    pub name: [u8; CLOCKWORK_PARAM_NAME_BYTES],
    pub taper: f32,
    pub role: u32,
    pub text: u32,
    pub text_bytes: u32,
    pub reserved: [u32; 2],
}

/// The front of the region.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct ClockworkParamsHeader {
    pub magic: u32,
    pub version: u32,
    pub capacity: u32,
    pub count: u32,
    pub generation: u32,
    pub values_offset: u32,
    pub entries_offset: u32,
    pub entry_bytes: u32,
    pub text_offset: u32,
    pub text_capacity: u32,
    pub text_used: u32,
    pub reserved: [u32; 5],
}

impl ClockworkParamEntry {
    /// An entry to define: the facts, with a name cut to what fits, on a
    /// character's edge; no unit, automatable, even, in no group and with no role
    /// until the calls below say otherwise.
    pub fn new(id: u32, name: &str, kind: u32, min: f32, max: f32, def: f32) -> Self {
        let mut e = Self {
            live: 0, id, kind, unit: CLOCKWORK_PARAM_UNIT_NONE, flags: CLOCKWORK_PARAM_AUTOMATABLE, group: 0,
            min, max, def, definer: 0,
            name: [0; CLOCKWORK_PARAM_NAME_BYTES],
            taper: CLOCKWORK_PARAM_TAPER_LINEAR, role: CLOCKWORK_PARAM_ROLE_NONE, text: 0, text_bytes: 0,
            reserved: [0; 2],
        };
        let mut n = name.len().min(CLOCKWORK_PARAM_NAME_BYTES - 1);
        while !name.is_char_boundary(n) { n -= 1; }
        e.name[..n].copy_from_slice(&name.as_bytes()[..n]);
        e
    }

    /// A group: a name that others name as their `group`.
    pub fn new_group(id: u32, name: &str) -> Self {
        Self::new(id, name, CLOCKWORK_PARAM_GROUP, 0.0, 0.0, 0.0).flags(0)
    }

    pub fn unit(mut self, unit: u32) -> Self { self.unit = unit; self }
    pub fn flags(mut self, flags: u32) -> Self { self.flags = flags; self }
    pub fn group(mut self, group: u32) -> Self { self.group = group; self }
    pub fn taper(mut self, taper: f32) -> Self { self.taper = taper; self }
    pub fn role(mut self, role: u32) -> Self { self.role = role; self }

    /// An entry that retires `id`.
    pub fn retire(id: u32) -> Self {
        Self::new(id, "", CLOCKWORK_PARAM_RETIRE, 0.0, 0.0, 0.0).flags(0)
    }

    /// A zeroed entry, for `clockwork_params_read_entry` to fill.
    pub fn empty() -> Self {
        Self::new(0, "", CLOCKWORK_PARAM_RETIRE, 0.0, 0.0, 0.0).flags(0)
    }

    /// The name, up to its NUL.
    pub fn name_str(&self) -> &str {
        let n = self.name.iter().position(|&b| b == 0).unwrap_or(CLOCKWORK_PARAM_NAME_BYTES);
        core::str::from_utf8(&self.name[..n]).unwrap_or("")
    }
}

unsafe extern "C" {
    /// Declare, replace or retire entries, all or none (`clockwork_params.h`).
    /// Before the core first ticks, or from the thread that ticks it.
    pub fn clockwork_param_define(
        core: *mut ClockworkCore, entries: *const ClockworkParamEntry, count: u32,
        text: *const u8, text_bytes: u32,
        definer: u32, slots: *mut u32, why: *mut *const c_char,
    ) -> i32;
    /// Write a slot's value as `origin`, from the thread that ticks the core.
    pub fn clockwork_param_set(core: *mut ClockworkCore, slot: u32, value: f32, origin: u32) -> c_int;
    /// A slot's value now, from any thread.
    pub fn clockwork_param_value(core: *const ClockworkCore, slot: u32) -> f32;
    /// The core's table, or null before it has an arena.
    pub fn clockwork_params_table(core: *const ClockworkCore) -> *const ClockworkParamsHeader;
    /// The generation a client sees.
    pub fn clockwork_params_generation(h: *const ClockworkParamsHeader) -> u32;
    /// A slot's facts, consistent: 1 taken, 0 free, -1 read again.
    pub fn clockwork_params_read_entry(h: *const ClockworkParamsHeader, slot: u32, out: *mut ClockworkParamEntry) -> c_int;
    /// A slot's text and facts, consistent: the length, 0 for none, -1 read again, -2 too small.
    pub fn clockwork_params_read_text(
        h: *const ClockworkParamsHeader, slot: u32, out: *mut u8, capacity: u32, entry: *mut ClockworkParamEntry,
    ) -> i32;
}
