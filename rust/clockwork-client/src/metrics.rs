// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The counters, read as a block.
//!
//! Field-by-field reads across a live writer give a set of numbers that
//! never existed together, so both tables come back as one snapshot, and
//! the snapshot answers by name ([`Metric`], [`NativeStat`]) or by word.

use core::fmt;

use clockwork_abi::client::clockwork_client_metrics;

use crate::client::{Client, RegionId};
use crate::error::Result;
use crate::metrics_schema::{Metric, NativeStat};

/// The most metrics words a snapshot holds. The engine reports fewer
/// (`Metrics::len`); the schema's highest offset is well inside this.
pub const METRICS_WORDS_MAX: usize = 128;

/// The most native-stat words a snapshot holds.
pub const NATIVE_STATS_WORDS_MAX: usize = 16;

/// One coherent reading of the engine's performance metrics.
#[derive(Clone, Copy)]
pub struct Metrics {
    words: [u32; METRICS_WORDS_MAX],
    len: usize,
}

impl Metrics {
    pub(crate) fn read(client: &Client) -> Metrics {
        let mut m = Metrics { words: [0; METRICS_WORDS_MAX], len: 0 };
        // SAFETY: a live handle, and `out` has METRICS_WORDS_MAX slots, which
        // is the most the call is allowed to write.
        let n = unsafe {
            clockwork_client_metrics(client.as_ptr(), m.words.as_mut_ptr(), METRICS_WORDS_MAX as u32)
        };
        m.len = (n as usize).min(METRICS_WORDS_MAX);
        m
    }

    /// How many words the engine reported.
    pub fn len(&self) -> usize {
        self.len
    }

    /// True when the engine reported nothing.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Every word, in offset order.
    pub fn as_slice(&self) -> &[u32] {
        &self.words[..self.len]
    }

    /// The word at `offset`, if the engine reported that many.
    pub fn word(&self, offset: usize) -> Option<u32> {
        self.as_slice().get(offset).copied()
    }

    /// A metric by name. `None` when the engine reports fewer words than
    /// the metric's offset — a context metric the web host merges in, say,
    /// which a native engine never writes.
    pub fn get(&self, metric: Metric) -> Option<u32> {
        self.word(metric.offset() as usize)
    }

    /// Every named metric the engine reported, with its value.
    pub fn iter(&self) -> impl Iterator<Item = (Metric, u32)> + '_ {
        Metric::ALL.iter().filter_map(move |&m| self.get(m).map(|v| (m, v)))
    }
}

impl fmt::Debug for Metrics {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut map = f.debug_map();
        for (m, v) in self.iter() {
            map.entry(&m.key(), &v);
        }
        map.finish()
    }
}

/// One coherent reading of the device layer's account of its own callback:
/// DSP load average and peak, and the callbacks that overran their budget.
/// Zeros on an engine no device drives.
#[derive(Clone, Copy)]
pub struct NativeStats {
    words: [u32; NATIVE_STATS_WORDS_MAX],
    len: usize,
}

impl NativeStats {
    pub(crate) fn read(client: &Client) -> Result<NativeStats> {
        let region = client.region(RegionId::NativeStats)?;
        let mut s = NativeStats { words: [0; NATIVE_STATS_WORDS_MAX], len: 0 };
        s.len = region.read_u32s(&mut s.words);
        Ok(s)
    }

    /// How many words the region holds.
    pub fn len(&self) -> usize {
        self.len
    }

    /// True when the region is empty.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Every word, in slot order.
    pub fn as_slice(&self) -> &[u32] {
        &self.words[..self.len]
    }

    /// The word in `slot`, if the region has that many.
    pub fn word(&self, slot: usize) -> Option<u32> {
        self.as_slice().get(slot).copied()
    }

    /// A stat by name.
    pub fn get(&self, stat: NativeStat) -> Option<u32> {
        self.word(stat.index() as usize)
    }

    /// Average DSP load as a percentage of the callback's time budget,
    /// smoothed over the last ~10 callbacks.
    pub fn cpu_avg_percent(&self) -> f32 {
        self.get(NativeStat::CpuAvgCenti).unwrap_or(0) as f32 / 100.0
    }

    /// Peak DSP load as a percentage of the callback's time budget; spikes
    /// show at once and decay.
    pub fn cpu_peak_percent(&self) -> f32 {
        self.get(NativeStat::CpuPeakCenti).unwrap_or(0) as f32 / 100.0
    }

    /// Callbacks that overran their time budget since boot. What a dropout
    /// IS, counted.
    pub fn overruns(&self) -> u32 {
        self.get(NativeStat::CbOverruns).unwrap_or(0)
    }

    /// Every named stat the region holds, with its value.
    pub fn iter(&self) -> impl Iterator<Item = (NativeStat, u32)> + '_ {
        NativeStat::ALL.iter().filter_map(move |&s| self.get(s).map(|v| (s, v)))
    }
}

impl fmt::Debug for NativeStats {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut map = f.debug_map();
        for (s, v) in self.iter() {
            map.entry(&s.key(), &v);
        }
        map.finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::metrics_schema::{FIELDS, NATIVE_STATS};

    #[test]
    fn the_schema_s_tables_are_in_order_and_fit_a_snapshot() {
        // The enums' `info` indexes the tables by position, which is only
        // right while the tables are sorted; and a snapshot must be able to
        // hold the highest offset the schema names.
        assert!(FIELDS.windows(2).all(|w| w[0].offset < w[1].offset));
        assert!(NATIVE_STATS.windows(2).all(|w| w[0].index + 1 == w[1].index));
        assert_eq!(NATIVE_STATS[0].index, 0);
        assert!((FIELDS.last().unwrap().offset as usize) < METRICS_WORDS_MAX);
        assert!(NATIVE_STATS.len() <= NATIVE_STATS_WORDS_MAX);
        for m in Metric::ALL {
            assert_eq!(m.info().offset, m.offset(), "{}", m.key());
        }
        for s in NativeStat::ALL {
            assert_eq!(s.info().index, s.index(), "{}", s.key());
        }
        assert_eq!(Metric::AudioSampleRate.key(), "audioSampleRate");
        assert_eq!(NativeStat::CbOverruns.key(), "cbOverruns");
        assert_eq!(NativeStat::CbOverruns.info().unit, "count");
    }

    #[test]
    fn a_snapshot_answers_by_name_and_none_past_what_was_reported() {
        let mut m = Metrics { words: [0; METRICS_WORDS_MAX], len: 44 };
        m.words[Metric::AudioSampleRate.offset() as usize] = 48_000;
        m.words[0] = 7;
        assert_eq!(m.get(Metric::AudioSampleRate), Some(48_000));
        assert_eq!(m.get(Metric::EngineProcessCount), Some(7));
        assert_eq!(m.get(Metric::AudioInputChannels), None); // offset 45, past len
        assert_eq!(m.iter().count(), Metric::ALL.iter().filter(|m| m.offset() < 44).count());
        assert!(format!("{m:?}").contains("\"audioSampleRate\": 48000"));

        let mut s = NativeStats { words: [0; NATIVE_STATS_WORDS_MAX], len: 6 };
        s.words[0] = 1_234;
        s.words[1] = 9_950;
        s.words[2] = 3;
        assert_eq!(s.cpu_avg_percent(), 12.34);
        assert_eq!(s.cpu_peak_percent(), 99.5);
        assert_eq!(s.overruns(), 3);
        assert_eq!(s.get(NativeStat::NrtInFlightUs), Some(0));
        assert_eq!(s.word(6), None);
    }
}
