// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * Shared metrics offset constants for Clockwork.
 *
 * These offsets correspond to the PerformanceMetrics struct in shared_memory.h.
 * All values are Uint32 array indices (not byte offsets).
 *
 * Layout designed for contiguous memcpy operations:
 * - [0-8]   engine (WASM + worklet writes)
 * - [9-10]  OSC Out (main thread)
 * - [11-14] OSC In (osc_in_worker)
 * - [15-16] Debug (emit_debug_osc, in audio_processor.cpp)
 * - [17-19] Ring buffer usage (WASM writes)
 * - [20-22] Ring buffer peak usage (WASM writes)
 * - [23-25] engine late timing diagnostics (WASM writes)
 * - [26]    Ring buffer direct write failures (OscChannel SAB mode)
 * - [27-38] Link (native-only; always 0 on web)
 * - [39-45] System info: version + audio config (shared C++, write-once)
 * - [46-49] ClockworkClock readouts: tempo/beat/phase/playing (shared C++, per block)
 * - [50+]   Context metrics (JS main-thread only; above the SAB region)
 */

// =============================================================================
// engine metrics [0-8] (written by WASM and JS worklet)
// =============================================================================
export const ENGINE_PROCESS_COUNT = 0;        // Audio process() callback count
export const ENGINE_MESSAGES_PROCESSED = 1;   // OSC messages processed by the engine
export const ENGINE_MESSAGES_DROPPED = 2;     // Messages dropped (various reasons)
export const ENGINE_SCHEDULER_DEPTH = 3;      // Current scheduler queue depth
export const ENGINE_SCHEDULER_PEAK_DEPTH = 4; // Peak scheduler queue depth
export const ENGINE_SCHEDULER_DROPPED = 5;    // Messages dropped due to scheduler overflow
export const ENGINE_SEQUENCE_GAPS = 6;        // Detected sequence gaps (missing messages)
export const ENGINE_WASM_ERRORS = 7;          // WASM execution errors (written by JS worklet)
export const ENGINE_SCHEDULER_LATES = 8;      // Bundles executed after their scheduled time (WASM)

// =============================================================================
// OSC Out metrics [9-10] (written by clockwork.js main thread)
// =============================================================================
export const OSC_OUT_MESSAGES_SENT = 9;   // OSC messages sent to the engine
export const OSC_OUT_BYTES_SENT = 10;     // Total bytes sent

// =============================================================================
// OSC In metrics [11-14] (written by osc_in_worker.js)
// =============================================================================
export const OSC_IN_MESSAGES_RECEIVED = 11; // OSC messages received from the engine
export const OSC_IN_BYTES_RECEIVED = 12;    // Total bytes received
export const OSC_IN_DROPPED_MESSAGES = 13;  // Messages dropped (sequence gaps/corruption)
export const OSC_IN_CORRUPTED = 14;         // Ring buffer message corruption detected

// =============================================================================
// Debug metrics [15-16] (written by emit_debug_osc in audio_processor.cpp,
// on every runtime)
// =============================================================================
export const DEBUG_MESSAGES_RECEIVED = 15;  // Debug messages from the engine
export const DEBUG_BYTES_RECEIVED = 16;     // Total debug bytes

// =============================================================================
// Ring buffer usage [17-19] (written by WASM during process())
// =============================================================================
export const IN_BUFFER_USED_BYTES = 17;     // Bytes used in IN buffer
export const OUT_BUFFER_USED_BYTES = 18;    // Bytes used in OUT buffer
export const NRT_OUT_BUFFER_USED_BYTES = 19;  // Bytes used in NRT-out buffer

// =============================================================================
// Ring buffer peak usage [20-22] (written by WASM during process())
// =============================================================================
export const IN_BUFFER_PEAK_BYTES = 20;     // Peak bytes used in IN buffer
export const OUT_BUFFER_PEAK_BYTES = 21;    // Peak bytes used in OUT buffer
export const NRT_OUT_BUFFER_PEAK_BYTES = 22;  // Peak bytes used in NRT-out buffer

// =============================================================================
// engine late timing diagnostics [23-25] (written by WASM during process())
// =============================================================================
export const ENGINE_SCHEDULER_MAX_LATE_MS = 23;    // Maximum lateness observed (ms)
export const ENGINE_SCHEDULER_LAST_LATE_MS = 24;   // Most recent late magnitude (ms)
export const ENGINE_SCHEDULER_LAST_LATE_TICK = 25; // Process count when last late occurred

// =============================================================================
// Ring buffer direct write failures [26] (written by OscChannel in SAB mode)
// =============================================================================
export const RING_BUFFER_DIRECT_WRITE_FAILS = 26;   // SAB mode only: direct IN-ring writes that failed (lock contention / full ring) and were dropped

// =============================================================================
// Link [27-38] — native-only; the web build never writes these (always 0).
// Mirror of PerformanceMetrics in shared_memory.h.
// =============================================================================
export const LINK_PEERS              = 27;  // connected Link peers
export const LINK_TEMPO_MBPM         = 28;  // tempo, milli-BPM (bpm * 1000)
export const LINK_BEAT_CENTI         = 29;  // beat position * 100
export const LINK_PHASE_CENTI        = 30;  // phase within quantum * 100
export const LINK_PLAYING            = 31;  // transport 0/1
export const LINK_AUDIO_IN_CHANNELS  = 32;  // active received channels
export const LINK_AUDIO_STREAM_RATE  = 33;  // received stream sample rate (Hz)
export const LINK_AUDIO_UNDERRUNS    = 34;  // receiver queue underrun events
export const LINK_AUDIO_BUFFERED_MS  = 35;  // receiver queue depth (ms)
export const LINK_AUDIO_DRIFT_PPM    = 36;  // read-rate deviation from 1.0 (ppm, signed)
export const LINK_AUDIO_PUBLISH      = 37;  // publishing enabled 0/1
export const LINK_AUDIO_SINKS        = 38;  // active output sinks

// =============================================================================
// System info [39-45] — cross-platform; written by shared C++ (init_memory),
// so these are real values on web AND native. Mirror of PerformanceMetrics
// in shared_memory.h; asserted via CLOCKWORK_ASSERT_METRIC.
// =============================================================================
export const CLOCKWORK_VERSION_MAJOR = 39;  // CLOCKWORK_VERSION_MAJOR
export const CLOCKWORK_VERSION_MINOR = 40;  // CLOCKWORK_VERSION_MINOR
export const CLOCKWORK_VERSION_PATCH = 41;  // CLOCKWORK_VERSION_PATCH
export const AUDIO_SAMPLE_RATE        = 42;  // output sample rate (Hz)
export const AUDIO_BLOCK_SIZE         = 43;  // block size (frames; 128 on web)
export const AUDIO_OUTPUT_CHANNELS    = 44;  // output bus channels
export const AUDIO_INPUT_CHANNELS     = 45;  // input bus channels

// =============================================================================
// ClockworkClock readouts [46-49] — cross-platform; written per block by
// publishClockMetrics() (ClockworkClock.cpp). Live on web and native alike.
// =============================================================================
export const CLOCK_TEMPO_MBPM  = 46;  // tempo, milli-BPM (bpm * 1000)
export const CLOCK_BEAT_CENTI  = 47;  // beat position * 100
export const CLOCK_PHASE_CENTI = 48;  // phase within quantum * 100
export const CLOCK_PLAYING     = 49;  // transport 0/1

// Slot 50 is reserved padding in the C++ struct so METRICS_SIZE stays a
// multiple of 8 (the following arena regions are 8-byte aligned). The merged
// array reuses index 50 for the first context metric (CTX_DRIFT_OFFSET_MS),
// which is written after the SAB copy, mirroring how context reuses the
// native-only Link slots.
export const METRICS_RESERVED  = 50;

// Number of metric slots copied from the SAB/snapshot buffer. The C++ struct
// has one extra trailing pad word (index 51) purely for 8-byte alignment that
// carries no value, so we stop the copy at the reserved slot.
export const SAB_METRICS_COUNT = METRICS_RESERVED + 1;  // 51

// =============================================================================
// Context metrics [50-61] (written by main thread into merged array only)
// These are NOT in the C++ PerformanceMetrics struct or SAB — MetricsReader
// writes them into the local merged Uint32Array. They start at index 50,
// reusing the reserved padding slot (the last meaningful SAB metric is
// CLOCK_PLAYING at 49); context is written after the SAB copy so it wins.
// =============================================================================
export const CTX_DRIFT_OFFSET_MS = 50;             // Clock drift (int32, ms)
export const CTX_CLOCK_OFFSET_MS = 51;             // Clock offset for multi-system sync (int32, ms)
export const CTX_AUDIO_CONTEXT_STATE = 52;          // Enum: 0=unknown,1=running,2=suspended,3=closed,4=interrupted
// 53-55 RESERVED. They held bufferPoolUsedBytes/AvailableBytes/Allocations —
// one engine's sample buffer pool, at fixed offsets in a guest-agnostic file.
// Guest metrics are declared by the guest now and live in the range below.
// Left as a hole rather than renumbered: these are read by offset.
// 56 RESERVED. It held loadedSynthDefs, a count of the definitions the client
// had cached so it could replay them after a reload. The count still exists —
// a product tracks what it loaded and puts it back through
// restoreClientState() — but it is one guest's noun, so it is declared by that
// guest in the reserved guest range rather than sitting at a fixed offset in a
// guest-agnostic file. Left as a hole for the same reason 53-55 are: these are
// read by offset.
export const CTX_ENGINE_SCHEDULER_CAPACITY = 57;   // Static from bufferConstants
export const CTX_IN_BUFFER_CAPACITY = 58;           // Static from bufferConstants
export const CTX_OUT_BUFFER_CAPACITY = 59;          // Static from bufferConstants
export const CTX_NRT_OUT_BUFFER_CAPACITY = 60;        // Static from bufferConstants
export const CTX_MODE = 61;                         // Enum: 0=sab, 1=postMessage

// =============================================================================
// Audio diagnostics [62-68] (written by main thread into merged array only)
// =============================================================================
export const CTX_GLITCH_COUNT = 62;          // Chrome only: playbackStats.fallbackFramesEvents
export const CTX_GLITCH_DURATION_MS = 63;    // Chrome only: playbackStats.fallbackFramesDuration * 1000 (ms int)
export const CTX_AVERAGE_LATENCY_US = 64;    // Chrome only: playbackStats.averageLatency * 1_000_000 (us int)
export const CTX_MAX_LATENCY_US = 65;        // Chrome only: playbackStats.maximumLatency * 1_000_000 (us int)
export const CTX_AUDIO_HEALTH_PCT = 66;      // Cross-browser: audio health percentage 0-100
export const CTX_TOTAL_FRAMES_DURATION_MS = 67; // Chrome only: playbackStats.totalFramesDuration * 1000 (ms int)
export const CTX_HAS_PLAYBACK_STATS = 68;    // 1 if Chrome playbackStats available, 0 otherwise

// =============================================================================
// Buffer pool growth metrics [69-72] (written by main thread into merged array only)
// =============================================================================
/*
 * THE GUEST'S OWN METRICS.
 *
 * A harness that ships one guest's counters at fixed offsets has to be edited
 * to admit another, because no two guests count the same things.
 *
 * So the range is reserved and unnamed here, and a guest DECLARES what it
 * puts in it — name, slot, type, description — the same way it declares its
 * declaration in guest_metrics.js. Clockwork merges those declarations into
 * the schema it reports and accepts values only for declared names; anything
 * undeclared is dropped rather than written somewhere arbitrary.
 *
 * A guest that declares nothing simply has no metrics of its own, and the
 * range stays zero.
 */
export const GUEST_METRICS_BASE = 69;
export const GUEST_METRICS_COUNT = 32;

// Merged array size (slots 0-49 from SAB/snapshot, 50-61 context, 62-68 audio, 69-72 buffer growth)
export const MERGED_ARRAY_SIZE = GUEST_METRICS_BASE + GUEST_METRICS_COUNT;  // 101
