// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// Nothing here checks it is in step with the schema; regenerate after any
// change to metrics_schema.js.
//
// Metric names, units and human-readable descriptions for the clockwork
// performance metrics, for use by native GUIs. Offsets index the
// PerformanceMetrics struct (see shared_memory.h); native-stat indices
// address the separate NATIVE_STATS segment.

#pragma once

#include <cstdint>
#include <cstring>

namespace clockwork {
namespace metrics_schema {

struct FieldInfo
{
    uint32_t offset;         // index into the metrics Uint32 array
    const char* key;         // schema key (stable identifier)
    const char* unit;        // unit of measurement ("" if none)
    const char* description; // human-readable description
};

inline constexpr FieldInfo kFields[] = {
    {  0, "engineProcessCount", "count", "Audio process() calls" },
    {  1, "engineMessagesProcessed", "count", "Messages drained from the IN ring and dispatched" },
    {  2, "engineMessagesDropped", "count", "Messages dropped (ring buffer full)" },
    {  3, "engineSchedulerDepth", "count", "Current scheduler queue depth" },
    {  4, "engineSchedulerPeakDepth", "count", "Peak scheduler queue depth (high water mark)" },
    {  5, "engineSchedulerDropped", "count", "Events dropped because the scheduler queue overflowed" },
    {  6, "engineSequenceGaps", "count", "Messages lost in transit from host to Clockwork" },
    {  7, "engineWasmErrors", "count", "WASM execution errors in audio worklet" },
    {  8, "engineSchedulerLates", "count", "Bundles executed after their scheduled time" },
    {  9, "oscOutMessagesSent", "count", "OSC messages sent from host to Clockwork" },
    { 10, "oscOutBytesSent", "bytes", "Total bytes sent from host to Clockwork" },
    { 11, "oscInMessagesReceived", "count", "OSC replies received from Clockwork" },
    { 12, "oscInBytesReceived", "bytes", "Total bytes received from Clockwork" },
    { 13, "oscInMessagesDropped", "count", "Replies lost in transit from Clockwork to host" },
    { 14, "oscInCorrupted", "count", "Corrupted messages detected in the ring buffer" },
    { 15, "debugMessagesReceived", "count", "Debug messages from Clockwork" },
    { 16, "debugBytesReceived", "bytes", "Debug bytes received" },
    { 17, "inBufferUsedBytes", "bytes", "Bytes used in IN ring buffer" },
    { 18, "outBufferUsedBytes", "bytes", "Bytes used in OUT ring buffer" },
    { 19, "nrtOutBufferUsedBytes", "bytes", "Bytes used in NRT-out ring buffer" },
    { 20, "inBufferPeakBytes", "bytes", "Peak bytes used in IN ring buffer" },
    { 21, "outBufferPeakBytes", "bytes", "Peak bytes used in OUT ring buffer" },
    { 22, "nrtOutBufferPeakBytes", "bytes", "Peak bytes used in NRT-out ring buffer" },
    { 23, "engineSchedulerMaxLateMs", "ms", "Maximum lateness observed in the scheduler (ms)" },
    { 24, "engineSchedulerLastLateMs", "ms", "Most recent late magnitude in the scheduler (ms)" },
    { 25, "engineSchedulerLastLateTick", "count", "Process count when the last scheduler late occurred" },
    { 26, "ringBufferDirectWriteFails", "count", "SAB mode only: direct IN-ring writes that lost the lock race or hit a full ring and were dropped (no fallback)" },
    { 27, "linkPeers", "count", "Connected Ableton Link peers on the network" },
    { 28, "linkTempoMbpm", "milliBpm", "Shared Link session tempo" },
    { 29, "linkBeatCenti", "centi", "Current Link beat position" },
    { 30, "linkPhaseCenti", "centi", "Phase within the Link quantum" },
    { 31, "linkPlaying", "bool", "Link transport playing (0/1)" },
    { 32, "linkAudioInChannels", "count", "Active received Link Audio channels" },
    { 33, "linkAudioStreamRate", "Hz", "Received Link Audio stream sample rate" },
    { 34, "linkAudioUnderruns", "count", "Receiver queue underruns (stream audio arrived too late to play)" },
    { 35, "linkAudioBufferedMs", "ms", "Received Link Audio queued in the receiver (ms)" },
    { 36, "linkAudioDriftPpm", "ppm", "Read-rate deviation from the sender's clock (parts per million)" },
    { 37, "linkAudioPublish", "bool", "Link Audio publishing enabled (0/1)" },
    { 38, "linkAudioSinks", "count", "Active Link Audio output sinks" },
    { 39, "clockworkVersionMajor", "count", "Clockwork major version" },
    { 40, "clockworkVersionMinor", "count", "Clockwork minor version" },
    { 41, "clockworkVersionPatch", "count", "Clockwork patch version" },
    { 42, "audioSampleRate", "Hz", "Output sample rate" },
    { 43, "audioBlockSize", "count", "Audio block size in frames per callback" },
    { 44, "audioOutputChannels", "count", "Output bus channels" },
    { 45, "audioInputChannels", "count", "Input bus channels" },
    { 46, "clockTempoMbpm", "milliBpm", "Tempo of the engine's internal ClockworkClock" },
    { 47, "clockBeatCenti", "centi", "Current ClockworkClock beat position" },
    { 48, "clockPhaseCenti", "centi", "Phase within the quantum" },
    { 49, "clockPlaying", "bool", "Transport playing (0/1)" },
    { 50, "driftOffsetMs", "ms", "Clock drift between AudioContext and wall clock" },
    { 51, "clockOffsetMs", "ms", "Clock offset for multi-system sync" },
    { 52, "audioContextState", "", "AudioContext state" },
    { 57, "engineSchedulerCapacity", "count", "Maximum scheduler queue size" },
    { 58, "inBufferCapacity", "bytes", "IN ring buffer capacity" },
    { 59, "outBufferCapacity", "bytes", "OUT ring buffer capacity" },
    { 60, "nrtOutBufferCapacity", "bytes", "NRT-out ring buffer capacity" },
    { 61, "mode", "", "Transport mode" },
    { 62, "glitchCount", "count", "Chrome only: audio underrun/glitch events" },
    { 63, "glitchDurationMs", "ms", "Chrome only: total silence from audio underruns" },
    { 64, "averageLatencyUs", "us", "Chrome only: average audio output latency" },
    { 65, "maxLatencyUs", "us", "Chrome only: maximum audio output latency" },
    { 66, "audioHealthPct", "%", "Cross-browser: fraction of expected audio frames delivered (100% = no issues)" },
    { 67, "totalFramesDurationMs", "ms", "Chrome only: total audio rendered duration" },
    { 68, "hasPlaybackStats", "bool", "1 if Chrome playbackStats API is available, 0 otherwise" },
};

struct NativeStatInfo
{
    uint32_t index;          // u32 slot within the NATIVE_STATS segment
    const char* key;
    const char* unit;
    const char* description;
};

inline constexpr NativeStatInfo kNativeStats[] = {
    { 0, "cpuAvgCenti", "centi", "Average DSP load: how much of each audio callback's time budget the render consumed, smoothed over the last ~10 callbacks. 100% means rendering ate the whole real-time deadline" },
    { 1, "cpuPeakCenti", "centi", "Peak DSP load: spikes show immediately, then decay ~5% per callback so they fade instead of pinning forever. Sustained values near 100% risk audible glitches" },
    { 2, "cbOverruns", "count", "Audio callbacks that overran their time budget" },
    { 3, "nrtMaxPassUs", "us", "Longest the control thread has spent handling one batch of commands since boot" },
    { 4, "nrtInFlightUs", "us", "How long the control thread has been stuck in the command it is handling right now. Anything but 0 means later commands, and every reply behind them, are waiting" },
    { 5, "nrtRecentWorstUs", "us", "Longest the control thread has spent handling one batch of commands in the last minute (unlike the since-boot worst, this decays back to quiet)" },
};

// Rows combining several metrics in one reading ("current | peak", ...).
struct CompositeInfo
{
    const char* key;
    const char* description;
};

inline constexpr CompositeInfo kComposites[] = {
    { "schedulerQueueCurrentPeak", "Current | peak scheduler queue depth" },
    { "schedulerLateWorstLast", "Worst | most recent late bundle execution (ms)" },
    { "debugCountBytes", "Debug messages from Clockwork (count and bytes)" },
    { "oscSentCountBytes", "Messages | bytes sent from host to Clockwork" },
    { "oscRecvCountBytes", "Messages | bytes received back from Clockwork" },
    { "inRingUsedPeak", "Used / peak bytes in the IN ring buffer (host to Clockwork)" },
    { "outRingUsedPeak", "Used / peak bytes in the OUT ring buffer (Clockwork replies to host)" },
    { "nrtRingUsedPeak", "Used / peak bytes in the NRT-out ring buffer (replies, notifications, debug)" },
    { "linkAudioChannelsRate", "Received Link Audio channels and their sample rate" },
    { "linkAudioPublishSinks", "Link Audio publishing state (1 = on) | active output sinks" },
    { "engineVersion", "Clockwork engine version" },
    { "busChannelsOutIn", "Output | input audio bus channels" },
    { "nrtWorstRecentBoot", "Worst control pass in the last minute | since boot (ms)" },
};

inline const char* descriptionForComposite(const char* key)
{
    for (const CompositeInfo& c : kComposites)
        if (std::strcmp(c.key, key) == 0)
            return c.description;
    return nullptr;
}

inline const char* descriptionForOffset(uint32_t offset)
{
    for (const FieldInfo& f : kFields)
        if (f.offset == offset)
            return f.description;
    return nullptr;
}

inline const char* descriptionForNativeStat(uint32_t index)
{
    for (const NativeStatInfo& f : kNativeStats)
        if (f.index == index)
            return f.description;
    return nullptr;
}

} // namespace metrics_schema
} // namespace clockwork
