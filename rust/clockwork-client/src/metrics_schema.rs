// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// test/metrics_schema.test.mjs fails when this file is out of step with the
// schema, as it does for the C++ twin, src/metrics_schema.h.
//
//! The metrics schema, by name.
//!
//! The engine's counters are a flat `u32` array a client reads as a block
//! ([`crate::Metrics`]); the device layer's own account of its callback is a
//! second, smaller one ([`crate::NativeStats`]). This module says what each
//! word means — the key, unit and description every GUI shows, the same words
//! the web component and a Qt panel use — and gives each table an enum so a
//! reading is asked for as `Metric::AudioSampleRate` rather than as word 42.
//!
//! Offsets are not dense: the schema numbers context metrics the web host
//! merges in after the engine's, and a native engine reports fewer words than
//! the highest offset here. `Metrics::get` answers `None` for a word the
//! engine did not report.

/// One performance metric: `offset` indexes the metrics `u32` array.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FieldInfo {
    pub offset: u32,
    /// The schema key: the stable identifier, shared with the web UI.
    pub key: &'static str,
    /// The unit of measurement; empty for a state.
    pub unit: &'static str,
    pub description: &'static str,
}

/// One native stat: `index` is the `u32` slot within the NATIVE_STATS region.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NativeStatInfo {
    pub index: u32,
    pub key: &'static str,
    pub unit: &'static str,
    pub description: &'static str,
}

/// A row combining several metrics in one reading ("current | peak", ...).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CompositeInfo {
    pub key: &'static str,
    pub description: &'static str,
}

/// Every performance metric, in offset order.
pub const FIELDS: &[FieldInfo] = &[
    FieldInfo { offset: 0, key: "engineProcessCount", unit: "count", description: "Audio process() calls" },
    FieldInfo { offset: 1, key: "engineMessagesProcessed", unit: "count", description: "Messages drained from the IN ring and dispatched" },
    FieldInfo { offset: 2, key: "engineMessagesDropped", unit: "count", description: "Messages dropped (ring buffer full)" },
    FieldInfo { offset: 3, key: "engineSchedulerDepth", unit: "count", description: "Current scheduler queue depth" },
    FieldInfo { offset: 4, key: "engineSchedulerPeakDepth", unit: "count", description: "Peak scheduler queue depth (high water mark)" },
    FieldInfo { offset: 5, key: "engineSchedulerDropped", unit: "count", description: "Events dropped because the scheduler queue overflowed" },
    FieldInfo { offset: 6, key: "engineSequenceGaps", unit: "count", description: "Messages lost in transit from host to Clockwork" },
    FieldInfo { offset: 7, key: "engineWasmErrors", unit: "count", description: "WASM execution errors in audio worklet" },
    FieldInfo { offset: 8, key: "engineSchedulerLates", unit: "count", description: "Bundles executed after their scheduled time" },
    FieldInfo { offset: 9, key: "oscOutMessagesSent", unit: "count", description: "OSC messages sent from host to Clockwork" },
    FieldInfo { offset: 10, key: "oscOutBytesSent", unit: "bytes", description: "Total bytes sent from host to Clockwork" },
    FieldInfo { offset: 11, key: "oscInMessagesReceived", unit: "count", description: "OSC replies received from Clockwork" },
    FieldInfo { offset: 12, key: "oscInBytesReceived", unit: "bytes", description: "Total bytes received from Clockwork" },
    FieldInfo { offset: 13, key: "oscInMessagesDropped", unit: "count", description: "Replies lost in transit from Clockwork to host" },
    FieldInfo { offset: 14, key: "oscInCorrupted", unit: "count", description: "Corrupted messages detected in the ring buffer" },
    FieldInfo { offset: 15, key: "debugMessagesReceived", unit: "count", description: "Debug messages from Clockwork" },
    FieldInfo { offset: 16, key: "debugBytesReceived", unit: "bytes", description: "Debug bytes received" },
    FieldInfo { offset: 17, key: "inBufferUsedBytes", unit: "bytes", description: "Bytes used in IN ring buffer" },
    FieldInfo { offset: 18, key: "outBufferUsedBytes", unit: "bytes", description: "Bytes used in OUT ring buffer" },
    FieldInfo { offset: 19, key: "nrtOutBufferUsedBytes", unit: "bytes", description: "Bytes used in NRT-out ring buffer" },
    FieldInfo { offset: 20, key: "inBufferPeakBytes", unit: "bytes", description: "Peak bytes used in IN ring buffer" },
    FieldInfo { offset: 21, key: "outBufferPeakBytes", unit: "bytes", description: "Peak bytes used in OUT ring buffer" },
    FieldInfo { offset: 22, key: "nrtOutBufferPeakBytes", unit: "bytes", description: "Peak bytes used in NRT-out ring buffer" },
    FieldInfo { offset: 23, key: "engineSchedulerMaxLateMs", unit: "ms", description: "Maximum lateness observed in the scheduler (ms)" },
    FieldInfo { offset: 24, key: "engineSchedulerLastLateMs", unit: "ms", description: "Most recent late magnitude in the scheduler (ms)" },
    FieldInfo { offset: 25, key: "engineSchedulerLastLateTick", unit: "count", description: "Process count when the last scheduler late occurred" },
    FieldInfo { offset: 26, key: "ringBufferDirectWriteFails", unit: "count", description: "SAB mode only: direct IN-ring writes that lost the lock race or hit a full ring and were dropped (no fallback)" },
    FieldInfo { offset: 27, key: "linkPeers", unit: "count", description: "Connected Ableton Link peers on the network" },
    FieldInfo { offset: 28, key: "linkTempoMbpm", unit: "milliBpm", description: "Shared Link session tempo" },
    FieldInfo { offset: 29, key: "linkBeatCenti", unit: "centi", description: "Current Link beat position" },
    FieldInfo { offset: 30, key: "linkPhaseCenti", unit: "centi", description: "Phase within the Link quantum" },
    FieldInfo { offset: 31, key: "linkPlaying", unit: "bool", description: "Link transport playing (0/1)" },
    FieldInfo { offset: 32, key: "linkAudioInChannels", unit: "count", description: "Active received Link Audio channels" },
    FieldInfo { offset: 33, key: "linkAudioStreamRate", unit: "Hz", description: "Received Link Audio stream sample rate" },
    FieldInfo { offset: 34, key: "linkAudioUnderruns", unit: "count", description: "Receiver queue underruns (stream audio arrived too late to play)" },
    FieldInfo { offset: 35, key: "linkAudioBufferedMs", unit: "ms", description: "Received Link Audio queued in the receiver (ms)" },
    FieldInfo { offset: 36, key: "linkAudioDriftPpm", unit: "ppm", description: "Read-rate deviation from the sender's clock (parts per million)" },
    FieldInfo { offset: 37, key: "linkAudioPublish", unit: "bool", description: "Link Audio publishing enabled (0/1)" },
    FieldInfo { offset: 38, key: "linkAudioSinks", unit: "count", description: "Active Link Audio output sinks" },
    FieldInfo { offset: 39, key: "clockworkVersionMajor", unit: "count", description: "Clockwork major version" },
    FieldInfo { offset: 40, key: "clockworkVersionMinor", unit: "count", description: "Clockwork minor version" },
    FieldInfo { offset: 41, key: "clockworkVersionPatch", unit: "count", description: "Clockwork patch version" },
    FieldInfo { offset: 42, key: "audioSampleRate", unit: "Hz", description: "Output sample rate" },
    FieldInfo { offset: 43, key: "audioBlockSize", unit: "count", description: "Audio block size in frames per callback" },
    FieldInfo { offset: 44, key: "audioOutputChannels", unit: "count", description: "Output bus channels" },
    FieldInfo { offset: 45, key: "audioInputChannels", unit: "count", description: "Input bus channels" },
    FieldInfo { offset: 46, key: "clockTempoMbpm", unit: "milliBpm", description: "Tempo of the engine's internal ClockworkClock" },
    FieldInfo { offset: 47, key: "clockBeatCenti", unit: "centi", description: "Current ClockworkClock beat position" },
    FieldInfo { offset: 48, key: "clockPhaseCenti", unit: "centi", description: "Phase within the quantum" },
    FieldInfo { offset: 49, key: "clockPlaying", unit: "bool", description: "Transport playing (0/1)" },
    FieldInfo { offset: 50, key: "driftOffsetMs", unit: "ms", description: "Clock drift between AudioContext and wall clock" },
    FieldInfo { offset: 51, key: "clockOffsetMs", unit: "ms", description: "Clock offset for multi-system sync" },
    FieldInfo { offset: 52, key: "audioContextState", unit: "", description: "AudioContext state" },
    FieldInfo { offset: 57, key: "engineSchedulerCapacity", unit: "count", description: "Maximum scheduler queue size" },
    FieldInfo { offset: 58, key: "inBufferCapacity", unit: "bytes", description: "IN ring buffer capacity" },
    FieldInfo { offset: 59, key: "outBufferCapacity", unit: "bytes", description: "OUT ring buffer capacity" },
    FieldInfo { offset: 60, key: "nrtOutBufferCapacity", unit: "bytes", description: "NRT-out ring buffer capacity" },
    FieldInfo { offset: 61, key: "mode", unit: "", description: "Transport mode" },
    FieldInfo { offset: 62, key: "glitchCount", unit: "count", description: "Chrome only: audio underrun/glitch events" },
    FieldInfo { offset: 63, key: "glitchDurationMs", unit: "ms", description: "Chrome only: total silence from audio underruns" },
    FieldInfo { offset: 64, key: "averageLatencyUs", unit: "us", description: "Chrome only: average audio output latency" },
    FieldInfo { offset: 65, key: "maxLatencyUs", unit: "us", description: "Chrome only: maximum audio output latency" },
    FieldInfo { offset: 66, key: "audioHealthPct", unit: "%", description: "Cross-browser: fraction of expected audio frames delivered (100% = no issues)" },
    FieldInfo { offset: 67, key: "totalFramesDurationMs", unit: "ms", description: "Chrome only: total audio rendered duration" },
    FieldInfo { offset: 68, key: "hasPlaybackStats", unit: "bool", description: "1 if Chrome playbackStats API is available, 0 otherwise" },
];

/// Every native stat, in index order.
pub const NATIVE_STATS: &[NativeStatInfo] = &[
    NativeStatInfo { index: 0, key: "cpuAvgCenti", unit: "centi", description: "Average DSP load: how much of each audio callback's time budget the render consumed, smoothed over the last ~10 callbacks. 100% means rendering ate the whole real-time deadline" },
    NativeStatInfo { index: 1, key: "cpuPeakCenti", unit: "centi", description: "Peak DSP load: spikes show immediately, then decay ~5% per callback so they fade instead of pinning forever. Sustained values near 100% risk audible glitches" },
    NativeStatInfo { index: 2, key: "cbOverruns", unit: "count", description: "Audio callbacks that overran their time budget" },
    NativeStatInfo { index: 3, key: "nrtMaxPassUs", unit: "us", description: "Longest the control thread has spent handling one batch of commands since boot" },
    NativeStatInfo { index: 4, key: "nrtInFlightUs", unit: "us", description: "How long the control thread has been stuck in the command it is handling right now. Anything but 0 means later commands, and every reply behind them, are waiting" },
    NativeStatInfo { index: 5, key: "nrtRecentWorstUs", unit: "us", description: "Longest the control thread has spent handling one batch of commands in the last minute (unlike the since-boot worst, this decays back to quiet)" },
];

/// The composite rows.
pub const COMPOSITES: &[CompositeInfo] = &[
    CompositeInfo { key: "schedulerQueueCurrentPeak", description: "Current | peak scheduler queue depth" },
    CompositeInfo { key: "schedulerLateWorstLast", description: "Worst | most recent late bundle execution (ms)" },
    CompositeInfo { key: "debugCountBytes", description: "Debug messages from Clockwork (count and bytes)" },
    CompositeInfo { key: "oscSentCountBytes", description: "Messages | bytes sent from host to Clockwork" },
    CompositeInfo { key: "oscRecvCountBytes", description: "Messages | bytes received back from Clockwork" },
    CompositeInfo { key: "inRingUsedPeak", description: "Used / peak bytes in the IN ring buffer (host to Clockwork)" },
    CompositeInfo { key: "outRingUsedPeak", description: "Used / peak bytes in the OUT ring buffer (Clockwork replies to host)" },
    CompositeInfo { key: "nrtRingUsedPeak", description: "Used / peak bytes in the NRT-out ring buffer (replies, notifications, debug)" },
    CompositeInfo { key: "linkAudioChannelsRate", description: "Received Link Audio channels and their sample rate" },
    CompositeInfo { key: "linkAudioPublishSinks", description: "Link Audio publishing state (1 = on) | active output sinks" },
    CompositeInfo { key: "engineVersion", description: "Clockwork engine version" },
    CompositeInfo { key: "busChannelsOutIn", description: "Output | input audio bus channels" },
    CompositeInfo { key: "nrtWorstRecentBoot", description: "Worst control pass in the last minute | since boot (ms)" },
];

/// The performance metrics by name. `m as u32` is the offset.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Metric {
    EngineProcessCount = 0,
    EngineMessagesProcessed = 1,
    EngineMessagesDropped = 2,
    EngineSchedulerDepth = 3,
    EngineSchedulerPeakDepth = 4,
    EngineSchedulerDropped = 5,
    EngineSequenceGaps = 6,
    EngineWasmErrors = 7,
    EngineSchedulerLates = 8,
    OscOutMessagesSent = 9,
    OscOutBytesSent = 10,
    OscInMessagesReceived = 11,
    OscInBytesReceived = 12,
    OscInMessagesDropped = 13,
    OscInCorrupted = 14,
    DebugMessagesReceived = 15,
    DebugBytesReceived = 16,
    InBufferUsedBytes = 17,
    OutBufferUsedBytes = 18,
    NrtOutBufferUsedBytes = 19,
    InBufferPeakBytes = 20,
    OutBufferPeakBytes = 21,
    NrtOutBufferPeakBytes = 22,
    EngineSchedulerMaxLateMs = 23,
    EngineSchedulerLastLateMs = 24,
    EngineSchedulerLastLateTick = 25,
    RingBufferDirectWriteFails = 26,
    LinkPeers = 27,
    LinkTempoMbpm = 28,
    LinkBeatCenti = 29,
    LinkPhaseCenti = 30,
    LinkPlaying = 31,
    LinkAudioInChannels = 32,
    LinkAudioStreamRate = 33,
    LinkAudioUnderruns = 34,
    LinkAudioBufferedMs = 35,
    LinkAudioDriftPpm = 36,
    LinkAudioPublish = 37,
    LinkAudioSinks = 38,
    ClockworkVersionMajor = 39,
    ClockworkVersionMinor = 40,
    ClockworkVersionPatch = 41,
    AudioSampleRate = 42,
    AudioBlockSize = 43,
    AudioOutputChannels = 44,
    AudioInputChannels = 45,
    ClockTempoMbpm = 46,
    ClockBeatCenti = 47,
    ClockPhaseCenti = 48,
    ClockPlaying = 49,
    DriftOffsetMs = 50,
    ClockOffsetMs = 51,
    AudioContextState = 52,
    EngineSchedulerCapacity = 57,
    InBufferCapacity = 58,
    OutBufferCapacity = 59,
    NrtOutBufferCapacity = 60,
    Mode = 61,
    GlitchCount = 62,
    GlitchDurationMs = 63,
    AverageLatencyUs = 64,
    MaxLatencyUs = 65,
    AudioHealthPct = 66,
    TotalFramesDurationMs = 67,
    HasPlaybackStats = 68,
}

impl Metric {
    /// Every metric, in offset order.
    pub const ALL: &'static [Metric] = &[Metric::EngineProcessCount, Metric::EngineMessagesProcessed, Metric::EngineMessagesDropped, Metric::EngineSchedulerDepth, Metric::EngineSchedulerPeakDepth, Metric::EngineSchedulerDropped, Metric::EngineSequenceGaps, Metric::EngineWasmErrors, Metric::EngineSchedulerLates, Metric::OscOutMessagesSent, Metric::OscOutBytesSent, Metric::OscInMessagesReceived, Metric::OscInBytesReceived, Metric::OscInMessagesDropped, Metric::OscInCorrupted, Metric::DebugMessagesReceived, Metric::DebugBytesReceived, Metric::InBufferUsedBytes, Metric::OutBufferUsedBytes, Metric::NrtOutBufferUsedBytes, Metric::InBufferPeakBytes, Metric::OutBufferPeakBytes, Metric::NrtOutBufferPeakBytes, Metric::EngineSchedulerMaxLateMs, Metric::EngineSchedulerLastLateMs, Metric::EngineSchedulerLastLateTick, Metric::RingBufferDirectWriteFails, Metric::LinkPeers, Metric::LinkTempoMbpm, Metric::LinkBeatCenti, Metric::LinkPhaseCenti, Metric::LinkPlaying, Metric::LinkAudioInChannels, Metric::LinkAudioStreamRate, Metric::LinkAudioUnderruns, Metric::LinkAudioBufferedMs, Metric::LinkAudioDriftPpm, Metric::LinkAudioPublish, Metric::LinkAudioSinks, Metric::ClockworkVersionMajor, Metric::ClockworkVersionMinor, Metric::ClockworkVersionPatch, Metric::AudioSampleRate, Metric::AudioBlockSize, Metric::AudioOutputChannels, Metric::AudioInputChannels, Metric::ClockTempoMbpm, Metric::ClockBeatCenti, Metric::ClockPhaseCenti, Metric::ClockPlaying, Metric::DriftOffsetMs, Metric::ClockOffsetMs, Metric::AudioContextState, Metric::EngineSchedulerCapacity, Metric::InBufferCapacity, Metric::OutBufferCapacity, Metric::NrtOutBufferCapacity, Metric::Mode, Metric::GlitchCount, Metric::GlitchDurationMs, Metric::AverageLatencyUs, Metric::MaxLatencyUs, Metric::AudioHealthPct, Metric::TotalFramesDurationMs, Metric::HasPlaybackStats];

    /// The word this metric is, in the metrics array.
    pub const fn offset(self) -> u32 {
        self as u32
    }

    /// Key, unit and description.
    pub const fn info(self) -> &'static FieldInfo {
        match self {
            Metric::EngineProcessCount => &FIELDS[0],
            Metric::EngineMessagesProcessed => &FIELDS[1],
            Metric::EngineMessagesDropped => &FIELDS[2],
            Metric::EngineSchedulerDepth => &FIELDS[3],
            Metric::EngineSchedulerPeakDepth => &FIELDS[4],
            Metric::EngineSchedulerDropped => &FIELDS[5],
            Metric::EngineSequenceGaps => &FIELDS[6],
            Metric::EngineWasmErrors => &FIELDS[7],
            Metric::EngineSchedulerLates => &FIELDS[8],
            Metric::OscOutMessagesSent => &FIELDS[9],
            Metric::OscOutBytesSent => &FIELDS[10],
            Metric::OscInMessagesReceived => &FIELDS[11],
            Metric::OscInBytesReceived => &FIELDS[12],
            Metric::OscInMessagesDropped => &FIELDS[13],
            Metric::OscInCorrupted => &FIELDS[14],
            Metric::DebugMessagesReceived => &FIELDS[15],
            Metric::DebugBytesReceived => &FIELDS[16],
            Metric::InBufferUsedBytes => &FIELDS[17],
            Metric::OutBufferUsedBytes => &FIELDS[18],
            Metric::NrtOutBufferUsedBytes => &FIELDS[19],
            Metric::InBufferPeakBytes => &FIELDS[20],
            Metric::OutBufferPeakBytes => &FIELDS[21],
            Metric::NrtOutBufferPeakBytes => &FIELDS[22],
            Metric::EngineSchedulerMaxLateMs => &FIELDS[23],
            Metric::EngineSchedulerLastLateMs => &FIELDS[24],
            Metric::EngineSchedulerLastLateTick => &FIELDS[25],
            Metric::RingBufferDirectWriteFails => &FIELDS[26],
            Metric::LinkPeers => &FIELDS[27],
            Metric::LinkTempoMbpm => &FIELDS[28],
            Metric::LinkBeatCenti => &FIELDS[29],
            Metric::LinkPhaseCenti => &FIELDS[30],
            Metric::LinkPlaying => &FIELDS[31],
            Metric::LinkAudioInChannels => &FIELDS[32],
            Metric::LinkAudioStreamRate => &FIELDS[33],
            Metric::LinkAudioUnderruns => &FIELDS[34],
            Metric::LinkAudioBufferedMs => &FIELDS[35],
            Metric::LinkAudioDriftPpm => &FIELDS[36],
            Metric::LinkAudioPublish => &FIELDS[37],
            Metric::LinkAudioSinks => &FIELDS[38],
            Metric::ClockworkVersionMajor => &FIELDS[39],
            Metric::ClockworkVersionMinor => &FIELDS[40],
            Metric::ClockworkVersionPatch => &FIELDS[41],
            Metric::AudioSampleRate => &FIELDS[42],
            Metric::AudioBlockSize => &FIELDS[43],
            Metric::AudioOutputChannels => &FIELDS[44],
            Metric::AudioInputChannels => &FIELDS[45],
            Metric::ClockTempoMbpm => &FIELDS[46],
            Metric::ClockBeatCenti => &FIELDS[47],
            Metric::ClockPhaseCenti => &FIELDS[48],
            Metric::ClockPlaying => &FIELDS[49],
            Metric::DriftOffsetMs => &FIELDS[50],
            Metric::ClockOffsetMs => &FIELDS[51],
            Metric::AudioContextState => &FIELDS[52],
            Metric::EngineSchedulerCapacity => &FIELDS[53],
            Metric::InBufferCapacity => &FIELDS[54],
            Metric::OutBufferCapacity => &FIELDS[55],
            Metric::NrtOutBufferCapacity => &FIELDS[56],
            Metric::Mode => &FIELDS[57],
            Metric::GlitchCount => &FIELDS[58],
            Metric::GlitchDurationMs => &FIELDS[59],
            Metric::AverageLatencyUs => &FIELDS[60],
            Metric::MaxLatencyUs => &FIELDS[61],
            Metric::AudioHealthPct => &FIELDS[62],
            Metric::TotalFramesDurationMs => &FIELDS[63],
            Metric::HasPlaybackStats => &FIELDS[64],
        }
    }

    /// The schema key.
    pub const fn key(self) -> &'static str {
        self.info().key
    }
}

/// The native stats by name. `s as u32` is the slot.
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum NativeStat {
    CpuAvgCenti = 0,
    CpuPeakCenti = 1,
    CbOverruns = 2,
    NrtMaxPassUs = 3,
    NrtInFlightUs = 4,
    NrtRecentWorstUs = 5,
}

impl NativeStat {
    /// Every native stat, in index order.
    pub const ALL: &'static [NativeStat] = &[NativeStat::CpuAvgCenti, NativeStat::CpuPeakCenti, NativeStat::CbOverruns, NativeStat::NrtMaxPassUs, NativeStat::NrtInFlightUs, NativeStat::NrtRecentWorstUs];

    /// The slot this stat is, in the NATIVE_STATS region.
    pub const fn index(self) -> u32 {
        self as u32
    }

    /// Key, unit and description.
    pub const fn info(self) -> &'static NativeStatInfo {
        match self {
            NativeStat::CpuAvgCenti => &NATIVE_STATS[0],
            NativeStat::CpuPeakCenti => &NATIVE_STATS[1],
            NativeStat::CbOverruns => &NATIVE_STATS[2],
            NativeStat::NrtMaxPassUs => &NATIVE_STATS[3],
            NativeStat::NrtInFlightUs => &NATIVE_STATS[4],
            NativeStat::NrtRecentWorstUs => &NATIVE_STATS[5],
        }
    }

    /// The schema key.
    pub const fn key(self) -> &'static str {
        self.info().key
    }
}
