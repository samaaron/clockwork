// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * Canonical Clockwork metrics schema — the single source of truth for
 * metric offsets, types, units and human-readable descriptions, shared by
 * every GUI that renders engine metrics (the <clockwork-metrics> web
 * component, a Qt metrics panel, clockwork-state, ...).
 *
 * Offsets [0-49] index the PerformanceMetrics struct in src/shared_memory.h;
 * [50+] are context metrics merged by MetricsReader on the JS main thread.
 * Fields marked `nativeOnly: true` have no web writer (always 0 on WASM).
 *
 * The `nativeStats` section describes the separate NATIVE_STATS shm segment
 * (see NATIVE_STAT_* in shared_memory.h); `index` is the u32 slot, not a
 * PerformanceMetrics offset.
 *
 * A C++ mirror is generated from this file for native GUIs:
 *   node scripts/gen-metrics-schema-header.mjs   →   src/metrics_schema.h
 * Regenerate (and commit the header) whenever this file changes.
 */

// Descriptions for readings that combine several metrics in one row
// (e.g. "current | peak"). Referenced by the web layout below and exported
// so native GUIs can label their equivalent rows with the same words.
const COMPOSITES = {
  schedulerQueueCurrentPeak: { description: 'Current | peak scheduler queue depth' },
  schedulerLateWorstLast:    { description: 'Worst | most recent late bundle execution (ms)' },
  debugCountBytes:           { description: 'Debug messages from Clockwork (count and bytes)' },
  oscSentCountBytes:         { description: 'Messages | bytes sent from host to Clockwork' },
  oscRecvCountBytes:         { description: 'Messages | bytes received back from Clockwork' },
  inRingUsedPeak:            { description: 'Used / peak bytes in the IN ring buffer (host to Clockwork)' },
  outRingUsedPeak:           { description: 'Used / peak bytes in the OUT ring buffer (Clockwork replies to host)' },
  nrtRingUsedPeak:           { description: 'Used / peak bytes in the NRT-out ring buffer (replies, notifications, debug)' },
  linkAudioChannelsRate:     { description: 'Received Link Audio channels and their sample rate' },
  linkAudioPublishSinks:     { description: 'Link Audio publishing state (1 = on) | active output sinks' },
  engineVersion:             { description: 'Clockwork engine version' },
  busChannelsOutIn:          { description: 'Output | input audio bus channels' },
  nrtWorstRecentBoot:        { description: 'Worst control pass in the last minute | since boot (ms)' },
};

export const METRICS_SCHEMA = {
  metrics: {
    // engine metrics [0-8]
    engineProcessCount:          { offset: 0,  type: 'counter',  unit: 'count', description: 'Audio process() calls' },
    engineMessagesProcessed:     { offset: 1,  type: 'counter',  unit: 'count', description: 'Messages drained from the IN ring and dispatched' },
    engineMessagesDropped:       { offset: 2,  type: 'counter',  unit: 'count', description: 'Messages dropped (ring buffer full)' },
    engineSchedulerDepth:        { offset: 3,  type: 'gauge',    unit: 'count', description: 'Current scheduler queue depth' },
    engineSchedulerPeakDepth:    { offset: 4,  type: 'gauge',    unit: 'count', description: 'Peak scheduler queue depth (high water mark)' },
    engineSchedulerDropped:      { offset: 5,  type: 'counter',  unit: 'count', description: 'Events dropped because the scheduler queue overflowed' },
    engineSequenceGaps:          { offset: 6,  type: 'counter',  unit: 'count', description: 'Messages lost in transit from host to Clockwork' },
    engineWasmErrors:            { offset: 7,  type: 'counter',  unit: 'count', description: 'WASM execution errors in audio worklet' },
    engineSchedulerLates:        { offset: 8,  type: 'counter',  unit: 'count', description: 'Bundles executed after their scheduled time' },

    // OSC Out metrics [9-10]
    oscOutMessagesSent:           { offset: 9,  type: 'counter',  unit: 'count', description: 'OSC messages sent from host to Clockwork' },
    oscOutBytesSent:              { offset: 10, type: 'counter',  unit: 'bytes', description: 'Total bytes sent from host to Clockwork' },

    // OSC In metrics [11-14]
    oscInMessagesReceived:        { offset: 11, type: 'counter',  unit: 'count', description: 'OSC replies received from Clockwork' },
    oscInBytesReceived:           { offset: 12, type: 'counter',  unit: 'bytes', description: 'Total bytes received from Clockwork' },
    oscInMessagesDropped:         { offset: 13, type: 'counter',  unit: 'count', description: 'Replies lost in transit from Clockwork to host' },
    oscInCorrupted:               { offset: 14, type: 'counter',  unit: 'count', description: 'Corrupted messages detected in the ring buffer' },

    // Debug metrics [15-16]
    debugMessagesReceived:        { offset: 15, type: 'counter',  unit: 'count', description: 'Debug messages from Clockwork' },
    debugBytesReceived:           { offset: 16, type: 'counter',  unit: 'bytes', description: 'Debug bytes received' },

    // Ring buffer usage [17-22]
    inBufferUsedBytes:            { offset: 17, type: 'gauge',    unit: 'bytes', description: 'Bytes used in IN ring buffer' },
    outBufferUsedBytes:           { offset: 18, type: 'gauge',    unit: 'bytes', description: 'Bytes used in OUT ring buffer' },
    nrtOutBufferUsedBytes:         { offset: 19, type: 'gauge',    unit: 'bytes', description: 'Bytes used in NRT-out ring buffer' },
    inBufferPeakBytes:            { offset: 20, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in IN ring buffer' },
    outBufferPeakBytes:           { offset: 21, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in OUT ring buffer' },
    nrtOutBufferPeakBytes:         { offset: 22, type: 'gauge',    unit: 'bytes', description: 'Peak bytes used in NRT-out ring buffer' },

    // engine late timing diagnostics [23-25]
    engineSchedulerMaxLateMs:    { offset: 23, type: 'gauge',    unit: 'ms',    description: 'Maximum lateness observed in the scheduler (ms)' },
    engineSchedulerLastLateMs:   { offset: 24, type: 'gauge',    unit: 'ms',    description: 'Most recent late magnitude in the scheduler (ms)' },
    engineSchedulerLastLateTick: { offset: 25, type: 'gauge',    unit: 'count', description: 'Process count when the last scheduler late occurred' },

    // Ring buffer direct write failures [26]
    ringBufferDirectWriteFails:   { offset: 26, type: 'counter',  unit: 'count', description: 'SAB mode only: direct IN-ring writes that lost the lock race or hit a full ring and were dropped (no fallback)' },

    // Link session [27-31] — native-only (no web writer; always 0 on WASM).
    // ClockworkClock session state lives in its own SAB region (ClockworkClockState),
    // not in PerformanceMetrics. Owned by engine.clock.
    linkPeers:                    { offset: 27, type: 'gauge',    unit: 'count',    nativeOnly: true, description: 'Connected Ableton Link peers on the network' },
    linkTempoMbpm:                { offset: 28, type: 'gauge',    unit: 'milliBpm', nativeOnly: true, description: 'Shared Link session tempo' },
    linkBeatCenti:                { offset: 29, type: 'gauge',    unit: 'centi',    nativeOnly: true, description: 'Current Link beat position' },
    linkPhaseCenti:               { offset: 30, type: 'gauge',    unit: 'centi',    nativeOnly: true, description: 'Phase within the Link quantum' },
    linkPlaying:                  { offset: 31, type: 'gauge',    unit: 'bool',     nativeOnly: true, description: 'Link transport playing (0/1)' },

    // Link Audio stream health [32-38] — native-only (no web writer)
    linkAudioInChannels:          { offset: 32, type: 'gauge',    unit: 'count', nativeOnly: true, description: 'Active received Link Audio channels' },
    linkAudioStreamRate:          { offset: 33, type: 'gauge',    unit: 'Hz',    nativeOnly: true, description: 'Received Link Audio stream sample rate' },
    linkAudioUnderruns:           { offset: 34, type: 'counter',  unit: 'count', nativeOnly: true, description: 'Receiver queue underruns (stream audio arrived too late to play)' },
    linkAudioBufferedMs:          { offset: 35, type: 'gauge',    unit: 'ms',    nativeOnly: true, description: 'Received Link Audio queued in the receiver (ms)' },
    linkAudioDriftPpm:            { offset: 36, type: 'gauge',    unit: 'ppm',   signed: true, nativeOnly: true, description: "Read-rate deviation from the sender's clock (parts per million)" },
    linkAudioPublish:             { offset: 37, type: 'gauge',    unit: 'bool',  nativeOnly: true, description: 'Link Audio publishing enabled (0/1)' },
    linkAudioSinks:               { offset: 38, type: 'gauge',    unit: 'count', nativeOnly: true, description: 'Active Link Audio output sinks' },

    // System info [39-45] — cross-platform; written by shared C++ at init.
    clockworkVersionMajor:       { offset: 39, type: 'constant', unit: 'count', description: 'Clockwork major version' },
    clockworkVersionMinor:       { offset: 40, type: 'constant', unit: 'count', description: 'Clockwork minor version' },
    clockworkVersionPatch:       { offset: 41, type: 'constant', unit: 'count', description: 'Clockwork patch version' },
    audioSampleRate:              { offset: 42, type: 'constant', unit: 'Hz',    description: 'Output sample rate' },
    audioBlockSize:               { offset: 43, type: 'constant', unit: 'count', description: 'Audio block size in frames per callback' },
    audioOutputChannels:          { offset: 44, type: 'constant', unit: 'count', description: 'Output bus channels' },
    audioInputChannels:           { offset: 45, type: 'constant', unit: 'count', description: 'Input bus channels' },

    // ClockworkClock readouts [46-49] — cross-platform; written per block.
    clockTempoMbpm:               { offset: 46, type: 'gauge',    unit: 'milliBpm', description: "Tempo of the engine's internal ClockworkClock" },
    clockBeatCenti:               { offset: 47, type: 'gauge',    unit: 'centi',    description: 'Current ClockworkClock beat position' },
    clockPhaseCenti:              { offset: 48, type: 'gauge',    unit: 'centi',    description: 'Phase within the quantum' },
    clockPlaying:                 { offset: 49, type: 'gauge',    unit: 'bool',     description: 'Transport playing (0/1)' },

    // Context metrics [50+] (main thread only)
    driftOffsetMs:                { offset: 50, type: 'gauge',    unit: 'ms',    signed: true, description: 'Clock drift between AudioContext and wall clock' },
    clockOffsetMs:                { offset: 51, type: 'gauge',    unit: 'ms',    signed: true, description: 'Clock offset for multi-system sync' },
    audioContextState:            { offset: 52, type: 'enum',     values: ['unknown', 'running', 'suspended', 'closed', 'interrupted'], description: 'AudioContext state' },
    engineSchedulerCapacity:     { offset: 57, type: 'constant', unit: 'count', description: 'Maximum scheduler queue size' },
    inBufferCapacity:             { offset: 58, type: 'constant', unit: 'bytes', description: 'IN ring buffer capacity' },
    outBufferCapacity:            { offset: 59, type: 'constant', unit: 'bytes', description: 'OUT ring buffer capacity' },
    nrtOutBufferCapacity:          { offset: 60, type: 'constant', unit: 'bytes', description: 'NRT-out ring buffer capacity' },
    mode:                         { offset: 61, type: 'enum',     values: ['sab', 'postMessage'], description: 'Transport mode' },

    // Audio diagnostics [62-68] (main thread, Chrome playbackStats + cross-browser health)
    glitchCount:                  { offset: 62, type: 'counter',  unit: 'count', description: 'Chrome only: audio underrun/glitch events' },
    glitchDurationMs:             { offset: 63, type: 'gauge',    unit: 'ms',    description: 'Chrome only: total silence from audio underruns' },
    averageLatencyUs:             { offset: 64, type: 'gauge',    unit: 'us',    description: 'Chrome only: average audio output latency' },
    maxLatencyUs:                 { offset: 65, type: 'gauge',    unit: 'us',    description: 'Chrome only: maximum audio output latency' },
    audioHealthPct:               { offset: 66, type: 'gauge',    unit: '%',     description: 'Cross-browser: fraction of expected audio frames delivered (100% = no issues)' },
    totalFramesDurationMs:        { offset: 67, type: 'counter',  unit: 'ms',    description: 'Chrome only: total audio rendered duration' },
    hasPlaybackStats:             { offset: 68, type: 'gauge',    unit: 'bool',  description: '1 if Chrome playbackStats API is available, 0 otherwise' },

    // Buffer pool growth metrics [69-72] (main thread)
  },

  // NATIVE_STATS shm segment (native/JUCE backend only; see NATIVE_STAT_* in
  // src/shared_memory.h). `index` is the u32 slot within the segment — a
  // separate address space from the PerformanceMetrics offsets above.
  nativeStats: {
    cpuAvgCenti:  { index: 0, type: 'gauge',   unit: 'centi', description: 'Average DSP load: how much of each audio callback\'s time budget the render consumed, smoothed over the last ~10 callbacks. 100% means rendering ate the whole real-time deadline' },
    cpuPeakCenti: { index: 1, type: 'gauge',   unit: 'centi', description: 'Peak DSP load: spikes show immediately, then decay ~5% per callback so they fade instead of pinning forever. Sustained values near 100% risk audible glitches' },
    cbOverruns:   { index: 2, type: 'counter', unit: 'count', description: 'Audio callbacks that overran their time budget' },
    nrtMaxPassUs:  { index: 3, type: 'gauge', unit: 'us', description: 'Longest the control thread has spent handling one batch of commands since boot' },
    nrtInFlightUs: { index: 4, type: 'gauge', unit: 'us', description: 'How long the control thread has been stuck in the command it is handling right now. Anything but 0 means later commands, and every reply behind them, are waiting' },
    nrtRecentWorstUs: { index: 5, type: 'gauge', unit: 'us', description: 'Longest the control thread has spent handling one batch of commands in the last minute (unlike the since-boot worst, this decays back to quiet)' },
  },

  composites: COMPOSITES,

  layout: {
    panels: [
      {
        title: 'OSC Out',
        rows: [
          { label: 'sent',   cells: [{ key: 'oscOutMessagesSent' }] },
          { label: 'bytes',  cells: [{ key: 'oscOutBytesSent', kind: 'muted', format: 'bytes' }] },
          { label: 'lost',   cells: [{ key: 'engineSequenceGaps', kind: 'error' }] },
        ]
      },
      {
        title: 'OSC In',
        rows: [
          { label: 'received',  cells: [{ key: 'oscInMessagesReceived' }] },
          { label: 'bytes',     cells: [{ key: 'oscInBytesReceived', kind: 'muted', format: 'bytes' }] },
          { label: 'dropped',   cells: [{ key: 'oscInMessagesDropped', kind: 'error' }] },
          { label: 'corrupted', cells: [{ key: 'oscInCorrupted', kind: 'error' }] },
        ]
      },
      {
        title: 'Engine Scheduler',
        rows: [
          { label: 'queue',   tooltip: COMPOSITES.schedulerQueueCurrentPeak.description, cells: [{ key: 'engineSchedulerDepth' }, { sep: ' | ' }, { key: 'engineSchedulerPeakDepth', kind: 'muted' }] },
          { label: 'dropped', cells: [{ key: 'engineSchedulerDropped', kind: 'error' }] },
          { label: 'lates',   cells: [{ key: 'engineSchedulerLates', kind: 'error' }] },
          { label: 'max | last', tooltip: COMPOSITES.schedulerLateWorstLast.description, cells: [{ key: 'engineSchedulerMaxLateMs', kind: 'error' }, { sep: ' | ' }, { key: 'engineSchedulerLastLateMs', kind: 'dim' }, { text: ' ms', kind: 'muted' }] },
        ]
      },
      {
        title: 'Engine',
        rows: [
          { label: 'ticks',       tooltip: 'Audio process() callback count and OSC messages processed', cells: [{ key: 'engineProcessCount', kind: 'dim' }, { sep: ' | ' }, { key: 'engineMessagesProcessed', kind: 'muted' }, { text: ' msgs', kind: 'muted' }] },
          { label: 'dropped',     cells: [{ key: 'engineMessagesDropped', kind: 'error' }] },
          { label: 'drift',       cells: [{ key: 'driftOffsetMs', format: 'signed' }, { text: ' ms', kind: 'muted' }] },
          { label: 'debug',       tooltip: COMPOSITES.debugCountBytes.description, cells: [{ key: 'debugMessagesReceived', kind: 'muted' }, { text: ' (' }, { key: 'debugBytesReceived', kind: 'muted', format: 'bytes' }, { text: ')' }] },
        ]
      },
      {
        title: 'Ring Buffer Level',
        class: 'wide',
        rows: [
          { type: 'bar', label: 'in',  usedKey: 'inBufferUsedBytes',  peakKey: 'inBufferPeakBytes',  capacityKey: 'inBufferCapacity',  color: 'blue' },
          { type: 'bar', label: 'out', usedKey: 'outBufferUsedBytes', peakKey: 'outBufferPeakBytes', capacityKey: 'outBufferCapacity', color: 'green' },
          { type: 'bar', label: 'dbg', usedKey: 'nrtOutBufferUsedBytes', peakKey: 'nrtOutBufferPeakBytes', capacityKey: 'nrtOutBufferCapacity', color: 'purple' },
          { label: 'direct write fails', cells: [{ key: 'ringBufferDirectWriteFails', kind: 'error' }] },
        ]
      },
      // A panel of one engine's sample-pool and definition counters used to
      // sit here. A panel is a claim about what a guest HAS, so it moved out
      // with the metrics it displays: a guest supplies its own through
      // `metricsPanels` on its profile, and one with no pool shows no panel
      // rather than four empty rows.
      {
        title: 'AudioWorklet',
        rows: [
          { label: 'health',      tooltip: 'AudioContext state and audio health percentage (fraction of expected frames delivered)', cells: [{ key: 'audioContextState', kind: 'green', format: 'enum' }, { sep: ' | ' }, { key: 'audioHealthPct', kind: 'green', format: 'percent' }, { text: ' %', kind: 'muted' }] },
          { label: 'glitches',    tooltip: 'Chrome only: audio underrun/glitch events and total silence duration', cells: [{ key: 'glitchCount', kind: 'error', format: 'chromeOnly' }, { sep: ' (' }, { key: 'glitchDurationMs', kind: 'error', format: 'chromeOnly' }, { text: ' ms)', kind: 'muted' }] },
          { label: 'latency',     tooltip: 'Chrome only: avg | max audio output latency in ms', cells: [{ key: 'averageLatencyUs', kind: 'dim', format: 'chromeLatencyUs' }, { sep: ' | ' }, { key: 'maxLatencyUs', kind: 'dim', format: 'chromeLatencyUs' }, { text: ' ms', kind: 'muted' }] },
          { label: 'WASM errors', cells: [{ key: 'engineWasmErrors', kind: 'error' }] },
        ]
      },
      {
        title: 'Engine',
        rows: [
          { label: 'version',  cells: [{ key: 'clockworkVersionMajor' }, { text: '.' }, { key: 'clockworkVersionMinor' }, { text: '.' }, { key: 'clockworkVersionPatch' }] },
          { label: 'rate',     cells: [{ key: 'audioSampleRate' }, { text: ' Hz', kind: 'muted' }] },
          { label: 'block',    cells: [{ key: 'audioBlockSize' }, { text: ' frames', kind: 'muted' }] },
          { label: 'channels', tooltip: COMPOSITES.busChannelsOutIn.description, cells: [{ key: 'audioOutputChannels' }, { sep: ' | ' }, { key: 'audioInputChannels', kind: 'muted' }] },
        ]
      },
      {
        title: 'Clock',
        rows: [
          { label: 'tempo',   cells: [{ key: 'clockTempoMbpm', format: 'milliBpm' }, { text: ' bpm', kind: 'muted' }] },
          { label: 'beat',    cells: [{ key: 'clockBeatCenti', kind: 'dim', format: 'centi' }] },
          { label: 'phase',   cells: [{ key: 'clockPhaseCenti', kind: 'dim', format: 'centi' }] },
          { label: 'playing', cells: [{ key: 'clockPlaying', kind: 'muted' }] },
        ]
      },
    ]
  },
};
