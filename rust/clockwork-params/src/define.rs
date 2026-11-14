// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Entries and their text as a define takes them: `clockwork_param_define`,
//! `DspHost::param_define`, and the two blobs of `/clockwork/param/define`.
use clockwork_abi::params::*;

/// One string of an entry's text.
#[derive(Clone, Copy, Debug)]
pub enum Say<'a> {
    /// The whole name, when the entry's had to be shortened.
    FullName(&'a str),
    /// What it does, for a person.
    Doc(&'a str),
    /// A custom unit's words.
    Unit(&'a str),
    /// One of a choice's names, in order.
    Value(&'a str),
}

/// A define being put together.
#[derive(Clone, Debug, Default)]
pub struct Define {
    entries: Vec<ClockworkParamEntry>,
    text: Vec<u8>,
}

impl Define {
    pub fn new() -> Self { Self::default() }

    /// One more entry, with what it says.
    pub fn add(&mut self, mut entry: ClockworkParamEntry, says: &[Say]) -> &mut Self {
        let start = self.text.len();
        for say in says {
            let (tag, words) = match *say {
                Say::FullName(w) => (CLOCKWORK_PARAM_TEXT_FULL_NAME, w),
                Say::Doc(w) => (CLOCKWORK_PARAM_TEXT_DOC, w),
                Say::Unit(w) => (CLOCKWORK_PARAM_TEXT_UNIT, w),
                Say::Value(w) => (CLOCKWORK_PARAM_TEXT_VALUE, w),
            };
            self.text.push(tag);
            self.text.extend_from_slice(words.as_bytes());
            self.text.push(0);
        }
        let bytes = self.text.len() - start;
        entry.text = if bytes == 0 { 0 } else { start as u32 };
        entry.text_bytes = bytes as u32;
        self.entries.push(entry);
        self
    }

    pub fn entries(&self) -> &[ClockworkParamEntry] { &self.entries }
    /// The blob the entries' text fields point into; empty when none has any.
    pub fn text(&self) -> &[u8] { &self.text }
    /// The entries as the verb's first blob.
    pub fn entry_bytes(&self) -> &[u8] {
        // SAFETY: an entry is plain words, and these are all of them.
        unsafe { core::slice::from_raw_parts(self.entries.as_ptr().cast::<u8>(), core::mem::size_of_val(self.entries.as_slice())) }
    }
}
