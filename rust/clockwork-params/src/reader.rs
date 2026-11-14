// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The region's bytes, parsed directly. The layout is `clockwork_abi::params`,
//! held to the C header by `clockwork-abi/tests/layout.rs`; the protocol is the
//! generation's, which docs/PARAMS.md states: read it, read the facts, read it
//! again, and read again if it moved.
use core::marker::PhantomData;
use core::mem::{offset_of, size_of};
use core::sync::atomic::{fence, AtomicU32, Ordering};

use clockwork_abi::params::*;

const ENTRY_BYTES: usize = size_of::<ClockworkParamEntry>();
const ENTRY_WORDS: usize = ENTRY_BYTES / 4;
const LIVE_AT: usize = offset_of!(ClockworkParamEntry, live);
const GENERATION_AT: usize = offset_of!(ClockworkParamsHeader, generation);

// A define is a bounded copy, so a few tries outlast one; a reader that loses
// every try keeps what it has, and its next refresh tries again.
const TRIES: u32 = 1024;

/// One entry's facts and text, as they stood at one generation.
#[derive(Clone, Debug)]
pub struct Param {
    pub(crate) slot: u32,
    pub(crate) entry: ClockworkParamEntry,
    pub(crate) text: Box<[u8]>,
}

impl Param {
    /// From an entry's bytes and its run, as `/clockwork/param/text` answers one
    /// or a web reader hands them over. None when the bytes are too few.
    pub fn from_bytes(entry: &[u8], text: &[u8]) -> Option<Self> {
        if entry.len() < ENTRY_BYTES {
            return None;
        }
        // SAFETY: within the slice, as just checked, and an entry is plain words,
        // for which every bit pattern is a value.
        let entry = unsafe { core::ptr::read_unaligned(entry.as_ptr().cast::<ClockworkParamEntry>()) };
        Some(Self { slot: 0, entry, text: text.into() })
    }

    /// Where its value is: [`Reader::value`] takes this.
    pub fn slot(&self) -> u32 { self.slot }
    /// All the facts, for what these calls do not name.
    pub fn entry(&self) -> &ClockworkParamEntry { &self.entry }
    pub fn id(&self) -> u32 { self.entry.id }
    pub fn is_group(&self) -> bool { self.entry.kind == CLOCKWORK_PARAM_GROUP }
    pub fn is_hidden(&self) -> bool { self.entry.flags & CLOCKWORK_PARAM_HIDDEN != 0 }
    pub fn is_read_only(&self) -> bool { self.entry.flags & CLOCKWORK_PARAM_READ_ONLY != 0 }

    /// The short name, up to 31 bytes.
    pub fn name(&self) -> &str { self.entry.name_str() }
    /// The whole name: the same as [`Param::name`] unless that had to be shortened.
    pub fn full_name(&self) -> &str { self.said(CLOCKWORK_PARAM_TEXT_FULL_NAME).unwrap_or_else(|| self.name()) }
    /// What it does, for a person.
    pub fn doc(&self) -> Option<&str> { self.said(CLOCKWORK_PARAM_TEXT_DOC) }
    /// A custom unit's words.
    pub fn unit_text(&self) -> Option<&str> { self.said(CLOCKWORK_PARAM_TEXT_UNIT) }
    /// A choice's names, in order.
    pub fn value_names(&self) -> impl Iterator<Item = &str> {
        self.strings().filter(|&(tag, _)| tag == CLOCKWORK_PARAM_TEXT_VALUE).map(|(_, words)| words)
    }
    /// One of a choice's names.
    pub fn value_name(&self, index: u32) -> Option<&str> { self.value_names().nth(index as usize) }

    fn said(&self, tag: u8) -> Option<&str> {
        self.strings().find(|&(t, _)| t == tag).map(|(_, words)| words)
    }

    // The run's strings: a tag, UTF-8 and a NUL each. A tag this reader does not
    // know is skipped by whoever asks; bytes that are no such string end the walk.
    fn strings(&self) -> impl Iterator<Item = (u8, &str)> {
        let mut rest: &[u8] = &self.text;
        core::iter::from_fn(move || {
            let (&tag, after) = rest.split_first()?;
            let end = after.iter().position(|&b| b == 0)?;
            let words = core::str::from_utf8(&after[..end]).ok()?;
            rest = &after[end + 1..];
            Some((tag, words))
        })
    }
}

/// A table's reader. [`Reader::refresh`] each frame costs one load unless the set
/// of entries moved; [`Reader::value`] is one load from the region. So a panel or
/// a visualiser draws from the memory itself and holds no state of its own.
pub struct Reader<'a> {
    base: *const u8,
    capacity: u32,
    values_at: usize,
    entries_at: usize,
    entry_bytes: usize,
    text_at: usize,
    text_bytes: usize,
    seen: Option<u32>,
    params: Vec<Param>,
    _memory: PhantomData<&'a [u8]>,
}

// SAFETY: every read of the region is an atomic load, whichever thread makes it.
unsafe impl Send for Reader<'_> {}

impl<'a> Reader<'a> {
    /// Over bytes that stay where they are for the reader's life: a web snapshot,
    /// a fixture. None when they are no table this reader knows.
    pub fn new(region: &'a [u8]) -> Option<Self> {
        // SAFETY: the slice is valid for its length for 'a, and only loaded from.
        unsafe { Self::from_raw(region.as_ptr(), region.len()) }
    }

    /// Over a region mapped into this process, by [`clockwork_client`]'s
    /// `Client::params` or any other way. None when it is no table this reader knows.
    ///
    /// # Safety
    /// `base` must be readable for `bytes` for as long as the reader lives, and
    /// whoever else touches those bytes must do so with atomic words, as the
    /// engine does. The memory may be mapped read-only.
    ///
    /// [`clockwork_client`]: https://docs.rs/clockwork-client
    pub unsafe fn from_raw(base: *const u8, bytes: usize) -> Option<Self> {
        if base.is_null() || (base as usize) % 4 != 0 || bytes < size_of::<ClockworkParamsHeader>() {
            return None;
        }
        let mut r = Self {
            base, capacity: 0, values_at: 0, entries_at: 0, entry_bytes: 0, text_at: 0, text_bytes: 0,
            seen: None, params: Vec::new(), _memory: PhantomData,
        };
        if r.word(offset_of!(ClockworkParamsHeader, magic)).load(Ordering::Acquire) != CLOCKWORK_PARAMS_MAGIC
            || r.field(offset_of!(ClockworkParamsHeader, version)) != CLOCKWORK_PARAMS_VERSION
        {
            return None;
        }
        r.capacity = r.field(offset_of!(ClockworkParamsHeader, capacity));
        r.values_at = r.field(offset_of!(ClockworkParamsHeader, values_offset)) as usize;
        r.entries_at = r.field(offset_of!(ClockworkParamsHeader, entries_offset)) as usize;
        r.entry_bytes = r.field(offset_of!(ClockworkParamsHeader, entry_bytes)) as usize;
        r.text_at = r.field(offset_of!(ClockworkParamsHeader, text_offset)) as usize;
        let text_capacity = r.field(offset_of!(ClockworkParamsHeader, text_capacity)) as usize;

        let on_a_word = |at: usize| at % 4 == 0;
        let slots = r.capacity as usize;
        let ends_within = |at: usize, each: usize| {
            slots.checked_mul(each).and_then(|n| at.checked_add(n)).is_some_and(|end| end <= bytes)
        };
        if r.entry_bytes < ENTRY_BYTES || !on_a_word(r.entry_bytes) || !on_a_word(r.values_at) || !on_a_word(r.entries_at)
            || !on_a_word(r.text_at) || !ends_within(r.values_at, 4) || !ends_within(r.entries_at, r.entry_bytes)
        {
            return None;
        }
        // The text may run past what was mapped, as a fixture's does: no run past
        // what is here is ever read.
        r.text_bytes = text_capacity.min(bytes.saturating_sub(r.text_at));
        Some(r)
    }

    fn word(&self, at: usize) -> &AtomicU32 {
        // SAFETY: every `at` is on a word and within what from_raw checked, the
        // memory is good for the reader's life, and it is only loaded from.
        unsafe { &*self.base.add(at).cast::<AtomicU32>() }
    }
    fn field(&self, at: usize) -> u32 { self.word(at).load(Ordering::Relaxed) }

    /// Read the facts again if the set of entries moved. True when it did and they
    /// were read; false when nothing moved, or when a define is under way and what
    /// was read stands until a later call. Allocates only when true.
    pub fn refresh(&mut self) -> bool {
        let generation = self.word(GENERATION_AT);
        if self.seen.is_some_and(|seen| seen == generation.load(Ordering::Acquire)) {
            return false;
        }
        for _ in 0..TRIES {
            let before = self.word(GENERATION_AT).load(Ordering::Acquire);
            if before & 1 != 0 {
                core::hint::spin_loop();
                continue;
            }
            let params = self.read_params();
            fence(Ordering::Acquire);
            if self.word(GENERATION_AT).load(Ordering::Relaxed) == before {
                self.params = params;
                self.seen = Some(before);
                return true;
            }
        }
        false
    }

    fn read_params(&self) -> Vec<Param> {
        let mut out = Vec::new();
        for slot in 0..self.capacity {
            let at = self.entries_at + slot as usize * self.entry_bytes;
            if self.field(at + LIVE_AT) == 0 {
                continue;
            }
            let mut words = [0u32; ENTRY_WORDS];
            for (w, word) in words.iter_mut().enumerate() {
                *word = self.field(at + w * 4);
            }
            // SAFETY: as many bytes as an entry, and an entry is plain words.
            let entry: ClockworkParamEntry = unsafe { core::mem::transmute(words) };
            out.push(Param { slot, entry, text: self.read_text(&entry) });
        }
        out
    }

    // An entry read mid-define may say anything, so nothing is read unless it lies
    // within the text; the generation then says whether to believe it.
    fn read_text(&self, e: &ClockworkParamEntry) -> Box<[u8]> {
        let (from, bytes) = (e.text as usize, e.text_bytes as usize);
        if bytes == 0 || from % 4 != 0 || from.checked_add(bytes).is_none_or(|end| end > self.text_bytes) {
            return Box::default();
        }
        let mut out = Vec::with_capacity(bytes);
        let mut done = 0;
        while done < bytes {
            let word = self.field(self.text_at + from + done).to_ne_bytes();
            let take = (bytes - done).min(4);
            out.extend_from_slice(&word[..take]);
            done += take;
        }
        out.into_boxed_slice()
    }

    /// The generation the facts here belong to: one more each time the set moved.
    /// 0 until a refresh has read them.
    pub fn generation(&self) -> u32 { self.seen.map_or(0, |seen| seen >> 1) }
    /// Every live entry, groups included, in slot order.
    pub fn params(&self) -> &[Param] { &self.params }
    pub fn find(&self, id: u32) -> Option<&Param> { self.params.iter().find(|p| p.id() == id) }
    /// Who holds a role; a table has at most one for each.
    pub fn with_role(&self, role: u32) -> Option<&Param> {
        if role == CLOCKWORK_PARAM_ROLE_NONE {
            return None;
        }
        self.params.iter().find(|p| p.entry.role == role)
    }
    /// A slot's value now, one load from the region. 0 for a slot outside the table.
    pub fn value(&self, slot: u32) -> f32 {
        if slot >= self.capacity {
            return 0.0;
        }
        f32::from_bits(self.field(self.values_at + slot as usize * 4))
    }
}
