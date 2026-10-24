// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//
// GENERATED FILE — DO NOT EDIT.
// Source of truth: js/lib/metrics_schema.js
// Regenerate with:  npm run gen:metrics-header
// test/metrics_schema.test.mjs fails when this file is out of step with the
// schema; regenerate after any change to metrics_schema.js.
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
    {  2, "engineMessagesDropped", "count", "Messages refused on the way in: the IN ring full, or a message too big for it" },
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
    { 34, "linkAudioUnderruns", "count", "Link Audio input frames that arrived too late to play, every input together" },
    { 35, "linkAudioBufferedMs", "ms", "Received Link Audio held ahead of playback, the most of any input (ms)" },
    { 36, "linkAudioDriftPpm", "ppm", "Read-rate deviation from the sender's clock (parts per million)" },
    { 37, "linkAudioPublish", "bool", "Link Audio publishing enabled (0/1)" },
    { 38, "linkAudioSinks", "count", "Active Link Audio output sinks" },
    { 39, "clockworkCommit", "commit", "Which Clockwork: the commit it was built from (its first 8 hex digits; 0 when unknown)" },
    { 40, "engineSchedulerCapacity", "count", "Timed events the scheduler can hold (0 in a build with none)" },
    { 41, "engineSchedulerPoolBytes", "bytes", "Bytes of timed messages the scheduler can hold (0 in a build with none)" },
    { 42, "audioSampleRate", "Hz", "Output sample rate" },
    { 43, "audioBlockSize", "count", "Audio block size in frames per callback" },
    { 44, "audioOutputChannels", "count", "Output bus channels" },
    { 45, "audioInputChannels", "count", "Input bus channels" },
    { 46, "clockTempoMbpm", "milliBpm", "Tempo of the engine's internal ClockworkClock" },
    { 47, "clockBeatCenti", "centi", "Current ClockworkClock beat position" },
    { 48, "clockPhaseCenti", "centi", "Phase within the quantum" },
    { 49, "clockPlaying", "bool", "Transport playing (0/1)" },
    { 50, "clockDriftUs", "us", "The difference between the audio clock and the wall clock (µs, signed), smoothed; a jump means the audio clock was re-anchored" },
    { 51, "clockOffsetMs", "ms", "Clock offset for multi-system sync" },
    { 52, "audioContextState", "", "AudioContext state" },
    { 53, "guestPoolUsedBytes", "bytes", "Bytes in use in the guest's pool now: what it has allocated from its bulk memory (payloads as carved)" },
    { 54, "guestPoolPeakBytes", "bytes", "The most bytes ever in use in the guest's pool" },
    { 55, "egressFramesRefused", "count", "Replies, broadcasts and log lines an egress ring refused, for want of room or for their size: what the clients never got. 0 in a healthy engine" },
    { 56, "rustHeapUsedBytes", "bytes", "Bytes in use in the rust heap now (the RT router's block)" },
    { 57, "rustHeapFallbacks", "count", "Rust allocations inside the engine that fell back to the system allocator because the rust heap was full" },
    { 58, "guestMallocBytes", "bytes", "Bytes the guest holds from malloc: taken inside its entry points with malloc or new and not freed" },
    { 59, "guestMallocsProcessing", "count", "malloc, calloc or realloc calls the guest made while processing (inside dsp_process, dsp_osc or dsp_asset) since boot; 0 for a guest that allocates only through clockwork" },
    { 60, "mallocsAfterBoot", "count", "malloc, calloc or realloc calls anywhere in the engine's program since the engine was ready; the memory model says 0" },
    { 61, "mallocBytesAfterBoot", "bytes", "Bytes taken from malloc since the engine was ready and still held" },
    { 62, "egressFramesUndelivered", "count", "Replies and broadcasts the transport could not deliver: a client gone, a datagram the socket refused, a peer whose reply ring was full. 0 in a healthy engine" },
    { 63, "clockAnchors", "count", "Times the audio clock was re-anchored since boot" },
    { 64, "engineSchedulerLateDropped", "count", "Timed events dropped for coming due later than the host allows (schedulerLateLimitMs); 0 unless a host set a limit" },
    { 80, "inBufferCapacity", "bytes", "IN ring buffer capacity" },
    { 81, "outBufferCapacity", "bytes", "OUT ring buffer capacity" },
    { 82, "nrtOutBufferCapacity", "bytes", "NRT-out ring buffer capacity" },
    { 83, "mode", "", "Transport mode" },
    { 84, "glitchCount", "count", "Chrome only: audio underrun/glitch events" },
    { 85, "glitchDurationMs", "ms", "Chrome only: total silence from audio underruns" },
    { 86, "averageLatencyUs", "us", "Chrome only: average audio output latency" },
    { 87, "maxLatencyUs", "us", "Chrome only: maximum audio output latency" },
    { 88, "audioHealthPct", "%", "Cross-browser: fraction of expected audio frames delivered (100% = no issues)" },
    { 89, "totalFramesDurationMs", "ms", "Chrome only: total audio rendered duration" },
    { 90, "hasPlaybackStats", "bool", "1 if Chrome playbackStats API is available, 0 otherwise" },
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

// ── The panel layout ───────────────────────────────────────────────────────
// The rows every metrics panel shows, as the <clockwork-metrics> web component
// renders them (js/lib/metrics_schema.js, layout): one layout, so a native
// panel and the web one cannot drift. A native panel shows the rows whose
// platform is Both or Native; a row's platform comes from the fields it reads
// (rowPlatform). Panels, rows and cells are flat tables, a panel naming its
// rows and a row its cells by range.
enum class Platform : uint8_t { Both, Web, Native };
enum class CellType : uint8_t { Metric, NativeStat, Text, Separator };

struct LayoutCell
{
    CellType type;
    uint32_t index;          // Metric: the offset; NativeStat: the slot; else 0
    const char* text;        // Text and Separator: what is shown; else ""
    const char* kind;        // "", "muted", "dim", "error" or "green"
    const char* format;      // "", "bytes", "signed", "milliBpm", "centi", "latencyUs", "commit", ...
};

struct LayoutRow
{
    const char* label;
    const char* tooltip;     // the row's own, or its first field's description
    Platform platform;
    bool bar;                // a ring's level: used and peak against its capacity
    uint32_t usedOffset;     // bar only
    uint32_t peakOffset;     // bar only
    const char* capacityKey; // bar only: the ring's capacity, by schema key
    uint32_t firstCell;      // into kLayoutCells
    uint32_t cellCount;
};

struct LayoutPanel
{
    const char* title;
    uint32_t firstRow;       // into kLayoutRows
    uint32_t rowCount;
};

inline constexpr LayoutCell kLayoutCells[] = {
    { CellType::Metric, 9, "", "", "" },
    { CellType::Metric, 10, "", "muted", "bytes" },
    { CellType::Metric, 6, "", "error", "" },
    { CellType::Metric, 11, "", "", "" },
    { CellType::Metric, 12, "", "muted", "bytes" },
    { CellType::Metric, 13, "", "error", "" },
    { CellType::Metric, 14, "", "error", "" },
    { CellType::Metric, 3, "", "", "" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 4, "", "muted", "" },
    { CellType::Metric, 5, "", "error", "" },
    { CellType::Metric, 8, "", "error", "" },
    { CellType::Metric, 64, "", "error", "" },
    { CellType::Metric, 23, "", "error", "" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 24, "", "dim", "" },
    { CellType::Text, 0, " ms", "muted", "" },
    { CellType::Metric, 0, "", "dim", "" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 1, "", "muted", "" },
    { CellType::Text, 0, " msgs", "muted", "" },
    { CellType::Metric, 2, "", "error", "" },
    { CellType::Metric, 55, "", "error", "" },
    { CellType::Metric, 62, "", "error", "" },
    { CellType::Metric, 50, "", "", "signed" },
    { CellType::Text, 0, " µs", "muted", "" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 63, "", "muted", "" },
    { CellType::Text, 0, " anchors", "muted", "" },
    { CellType::Metric, 15, "", "muted", "" },
    { CellType::Text, 0, " (", "", "" },
    { CellType::Metric, 16, "", "muted", "bytes" },
    { CellType::Text, 0, ")", "", "" },
    { CellType::Metric, 26, "", "error", "" },
    { CellType::Metric, 52, "", "green", "enum" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 88, "", "green", "percent" },
    { CellType::Text, 0, " %", "muted", "" },
    { CellType::Metric, 84, "", "error", "chromeOnly" },
    { CellType::Separator, 0, " (", "", "" },
    { CellType::Metric, 85, "", "error", "chromeOnly" },
    { CellType::Text, 0, " ms)", "muted", "" },
    { CellType::Metric, 86, "", "dim", "chromeLatencyUs" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 87, "", "dim", "chromeLatencyUs" },
    { CellType::Text, 0, " ms", "muted", "" },
    { CellType::Metric, 7, "", "error", "" },
    { CellType::Metric, 39, "", "", "commit" },
    { CellType::Metric, 42, "", "", "" },
    { CellType::Text, 0, " Hz", "muted", "" },
    { CellType::Metric, 43, "", "", "" },
    { CellType::Text, 0, " frames", "muted", "" },
    { CellType::Metric, 44, "", "", "" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 45, "", "muted", "" },
    { CellType::Metric, 46, "", "", "milliBpm" },
    { CellType::Text, 0, " bpm", "muted", "" },
    { CellType::Metric, 47, "", "dim", "centi" },
    { CellType::Metric, 48, "", "dim", "centi" },
    { CellType::Metric, 49, "", "muted", "" },
    { CellType::Metric, 27, "", "", "" },
    { CellType::NativeStat, 0, "", "", "centi" },
    { CellType::Text, 0, "%", "muted", "" },
    { CellType::NativeStat, 1, "", "dim", "centi" },
    { CellType::Text, 0, "%", "muted", "" },
    { CellType::NativeStat, 2, "", "error", "" },
    { CellType::NativeStat, 4, "", "error", "latencyUs" },
    { CellType::Text, 0, " ms", "muted", "" },
    { CellType::NativeStat, 5, "", "", "latencyUs" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::NativeStat, 3, "", "muted", "latencyUs" },
    { CellType::Text, 0, " ms", "muted", "" },
    { CellType::Metric, 32, "", "", "" },
    { CellType::Text, 0, " ch @ ", "muted", "" },
    { CellType::Metric, 33, "", "muted", "" },
    { CellType::Text, 0, " Hz", "muted", "" },
    { CellType::Metric, 34, "", "error", "" },
    { CellType::Metric, 35, "", "dim", "" },
    { CellType::Text, 0, " ms", "muted", "" },
    { CellType::Metric, 36, "", "dim", "signed" },
    { CellType::Text, 0, " ppm", "muted", "" },
    { CellType::Metric, 37, "", "", "" },
    { CellType::Separator, 0, " | ", "", "" },
    { CellType::Metric, 38, "", "muted", "" },
    { CellType::Text, 0, " sinks", "muted", "" },
};

inline constexpr LayoutRow kLayoutRows[] = {
    { "sent", "OSC messages sent from host to Clockwork", Platform::Both, false, 0, 0, "", 0, 1 },
    { "bytes", "Total bytes sent from host to Clockwork", Platform::Both, false, 0, 0, "", 1, 1 },
    { "lost", "Messages lost in transit from host to Clockwork", Platform::Both, false, 0, 0, "", 2, 1 },
    { "received", "OSC replies received from Clockwork", Platform::Both, false, 0, 0, "", 3, 1 },
    { "bytes", "Total bytes received from Clockwork", Platform::Both, false, 0, 0, "", 4, 1 },
    { "dropped", "Replies lost in transit from Clockwork to host", Platform::Web, false, 0, 0, "", 5, 1 },
    { "corrupted", "Corrupted messages detected in the ring buffer", Platform::Both, false, 0, 0, "", 6, 1 },
    { "queue", "Current | peak scheduler queue depth", Platform::Both, false, 0, 0, "", 7, 3 },
    { "dropped", "Events dropped because the scheduler queue overflowed", Platform::Both, false, 0, 0, "", 10, 1 },
    { "lates", "Bundles executed after their scheduled time", Platform::Both, false, 0, 0, "", 11, 1 },
    { "late, dropped", "Timed events dropped for coming due later than the host allows; 0 unless a host set a limit", Platform::Both, false, 0, 0, "", 12, 1 },
    { "max | last", "Worst | most recent late bundle execution (ms)", Platform::Both, false, 0, 0, "", 13, 4 },
    { "ticks", "Audio process() callback count and OSC messages processed", Platform::Both, false, 0, 0, "", 17, 4 },
    { "dropped", "Messages refused on the way in", Platform::Both, false, 0, 0, "", 21, 1 },
    { "refused out", "Replies, broadcasts and log lines an egress ring refused, for want of room or for their size: what the clients never got", Platform::Both, false, 0, 0, "", 22, 1 },
    { "undelivered", "Replies and broadcasts the transport could not deliver: a client gone, a datagram the socket refused, a peer whose reply ring was full", Platform::Native, false, 0, 0, "", 23, 1 },
    { "clock", "The difference between the audio clock and the wall clock (µs, signed) and number of times it was re-anchored since boot", Platform::Both, false, 0, 0, "", 24, 5 },
    { "debug", "Debug messages from Clockwork (count and bytes)", Platform::Both, false, 0, 0, "", 29, 4 },
    { "in", "Bytes used in IN ring buffer", Platform::Both, true, 17, 20, "inBufferCapacity", 33, 0 },
    { "out", "Bytes used in OUT ring buffer", Platform::Both, true, 18, 21, "outBufferCapacity", 33, 0 },
    { "dbg", "Bytes used in NRT-out ring buffer", Platform::Both, true, 19, 22, "nrtOutBufferCapacity", 33, 0 },
    { "direct write fails", "SAB mode only: direct IN-ring writes that lost the lock race or hit a full ring and were dropped (no fallback)", Platform::Web, false, 0, 0, "", 33, 1 },
    { "health", "AudioContext state and audio health percentage (fraction of expected frames delivered)", Platform::Web, false, 0, 0, "", 34, 4 },
    { "glitches", "Chrome only: audio underrun/glitch events and total silence duration", Platform::Web, false, 0, 0, "", 38, 4 },
    { "latency", "Chrome only: avg | max audio output latency in ms", Platform::Web, false, 0, 0, "", 42, 4 },
    { "WASM errors", "WASM execution errors in audio worklet", Platform::Web, false, 0, 0, "", 46, 1 },
    { "clockwork", "Which Clockwork: the commit it was built from (its first 8 hex digits; 0 when unknown)", Platform::Both, false, 0, 0, "", 47, 1 },
    { "rate", "Output sample rate", Platform::Both, false, 0, 0, "", 48, 2 },
    { "block", "Audio block size in frames per callback", Platform::Both, false, 0, 0, "", 50, 2 },
    { "channels", "Output | input audio bus channels", Platform::Both, false, 0, 0, "", 52, 3 },
    { "tempo", "Tempo of the engine's internal ClockworkClock", Platform::Both, false, 0, 0, "", 55, 2 },
    { "beat", "Current ClockworkClock beat position", Platform::Both, false, 0, 0, "", 57, 1 },
    { "phase", "Phase within the quantum", Platform::Both, false, 0, 0, "", 58, 1 },
    { "playing", "Transport playing (0/1)", Platform::Both, false, 0, 0, "", 59, 1 },
    { "peers", "Connected Ableton Link peers on the network", Platform::Native, false, 0, 0, "", 60, 1 },
    { "load", "Average DSP load: how much of each audio callback's time budget the render consumed, smoothed over the last ~10 callbacks. 100% means rendering ate the whole real-time deadline", Platform::Native, false, 0, 0, "", 61, 2 },
    { "peak", "Peak DSP load: spikes show immediately, then decay ~5% per callback so they fade instead of pinning forever. Sustained values near 100% risk audible glitches", Platform::Native, false, 0, 0, "", 63, 2 },
    { "overruns", "Audio callbacks that overran their time budget", Platform::Native, false, 0, 0, "", 65, 1 },
    { "blocked", "How long the control thread has been stuck in the command it is handling right now. Anything but 0 means later commands, and every reply behind them, are waiting", Platform::Native, false, 0, 0, "", 66, 2 },
    { "worst", "Worst control pass in the last minute | since boot (ms)", Platform::Native, false, 0, 0, "", 68, 4 },
    { "in", "Received Link Audio channels and their sample rate", Platform::Native, false, 0, 0, "", 72, 4 },
    { "underruns", "Link Audio input frames that arrived too late to play, every input together", Platform::Native, false, 0, 0, "", 76, 1 },
    { "buffered", "Received Link Audio held ahead of playback, the most of any input (ms)", Platform::Native, false, 0, 0, "", 77, 2 },
    { "drift", "Read-rate deviation from the sender's clock (parts per million)", Platform::Native, false, 0, 0, "", 79, 2 },
    { "publish", "Link Audio publishing state (1 = on) | active output sinks", Platform::Native, false, 0, 0, "", 81, 4 },
};

inline constexpr LayoutPanel kLayoutPanels[] = {
    { "OSC Out", 0, 3 },
    { "OSC In", 3, 4 },
    { "Engine Scheduler", 7, 5 },
    { "Engine", 12, 6 },
    { "Ring Buffer Level", 18, 4 },
    { "AudioWorklet", 22, 4 },
    { "Audio", 26, 4 },
    { "Clock", 30, 5 },
    { "Performance", 35, 5 },
    { "Link Audio", 40, 5 },
};

} // namespace metrics_schema
} // namespace clockwork
