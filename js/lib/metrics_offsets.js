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
export const LINK_AUDIO_UNDERRUNS    = 34;  // Link Audio input frames that arrived too late to play
export const LINK_AUDIO_BUFFERED_MS  = 35;  // Link Audio held ahead of playback, the most of any input (ms)
export const LINK_AUDIO_DRIFT_PPM    = 36;  // read-rate deviation from 1.0 (ppm, signed)
export const LINK_AUDIO_PUBLISH      = 37;  // publishing enabled 0/1
export const LINK_AUDIO_SINKS        = 38;  // active output sinks

// =============================================================================
// System info [39-45] — cross-platform; written by shared C++ (init_memory),
// so these are real values on web AND native. Mirror of PerformanceMetrics
// in shared_memory.h; asserted via CLOCKWORK_ASSERT_METRIC.
// =============================================================================
export const CLOCKWORK_COMMIT         = 39;  // CLOCKWORK_COMMIT_WORD: which Clockwork, its commit's first 8 hex digits (0: unknown)
export const SCHEDULER_CAPACITY       = 40;  // timed events the scheduler can hold (0: no scheduler)
export const SCHEDULER_POOL_BYTES     = 41;  // bytes of timed messages the scheduler can hold (0: no scheduler)
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
// Every word of the C++ struct, the heap's census at 53-57, the guest's
// mallocs at 58-59 and the program's at 60-61 included: what the reader
// copies from the SAB before the context writes its own slots over 50-52
// (METRICS_SIZE / 4 in shared_memory.h).
export const SAB_METRICS_WORDS = 62;   // MALLOC_BYTES_AFTER_BOOT + 1

// =============================================================================
// Context metrics [50-63] (written by main thread into merged array only)
// These are NOT in the C++ PerformanceMetrics struct or SAB — MetricsReader
// writes them into the local merged Uint32Array. They start at index 50,
// reusing the reserved padding slot (the last meaningful SAB metric is
// CLOCK_PLAYING at 49); context is written after the SAB copy so it wins.
// =============================================================================
export const CTX_DRIFT_OFFSET_MS = 50;             // Clock drift (int32, ms)
export const CTX_CLOCK_OFFSET_MS = 51;             // Clock offset for multi-system sync (int32, ms)
export const CTX_AUDIO_CONTEXT_STATE = 52;          // Enum: 0=unknown,1=running,2=suspended,3=closed,4=interrupted
// 53-57 are the guest's pool's and the rust heap's census, in the C++ struct
// and the SAB (written by the engine at its metrics flush): the slots that
// once held the buffer pool's metrics, the loaded synthdef count and the
// scheduler's capacity, holes until now. The reader copies them from the SAB
// with the rest of the struct (SAB_METRICS_WORDS).
export const GUEST_POOL_USED_BYTES = 53;  // bytes in use in the guest's pool now (its bulk memory)
export const GUEST_POOL_PEAK_BYTES = 54;  // the most ever
export const METRICS_RESERVED3     = 55;  // reserved: held the pool's growth count until the pool stopped growing (2026-10-15)
export const RUST_HEAP_USED_BYTES = 56;   // bytes in use in the RT router's block
export const RUST_HEAP_FALLBACKS  = 57;   // Rust allocations inside the engine that fell back to the system allocator
// 58-59 are the guest's mallocs (src/guest_malloc.h), in the C++ struct and
// the SAB: counted where the link lets clockwork wrap the allocator (the
// web), 0 everywhere else.
export const GUEST_MALLOC_BYTES       = 58;   // bytes the guest holds from malloc, taken inside its entry points
export const GUEST_MALLOCS_PROCESSING = 59;   // malloc/calloc/realloc calls inside dsp_process, dsp_osc or dsp_asset since boot
// 60-61 are the whole program's mallocs after boot, any scope (src/guest_malloc.h): what reaches the system
// allocator once the engine is up, which should be nothing. Counted on the web, 0 everywhere else.
export const MALLOCS_AFTER_BOOT      = 60;   // malloc/calloc/realloc calls since the engine was ready
export const MALLOC_BYTES_AFTER_BOOT = 61;   // bytes taken since then and still held
export const CTX_IN_BUFFER_CAPACITY = 62;           // Static from bufferConstants
export const CTX_OUT_BUFFER_CAPACITY = 63;          // Static from bufferConstants
export const CTX_NRT_OUT_BUFFER_CAPACITY = 64;        // Static from bufferConstants
export const CTX_MODE = 65;                         // Enum: 0=sab, 1=postMessage

// =============================================================================
// Audio diagnostics [66-72] (written by main thread into merged array only)
// =============================================================================
export const CTX_GLITCH_COUNT = 66;          // Chrome only: playbackStats.fallbackFramesEvents
export const CTX_GLITCH_DURATION_MS = 67;    // Chrome only: playbackStats.fallbackFramesDuration * 1000 (ms int)
export const CTX_AVERAGE_LATENCY_US = 68;    // Chrome only: playbackStats.averageLatency * 1_000_000 (us int)
export const CTX_MAX_LATENCY_US = 69;        // Chrome only: playbackStats.maximumLatency * 1_000_000 (us int)
export const CTX_AUDIO_HEALTH_PCT = 70;      // Cross-browser: audio health percentage 0-100
export const CTX_TOTAL_FRAMES_DURATION_MS = 71; // Chrome only: playbackStats.totalFramesDuration * 1000 (ms int)
export const CTX_HAS_PLAYBACK_STATS = 72;    // 1 if Chrome playbackStats available, 0 otherwise

// =============================================================================
// The guest's metrics [71+] (written by main thread into merged array only)
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
export const GUEST_METRICS_BASE = 73;
export const GUEST_METRICS_COUNT = 32;

// Merged array size (slots 0-59 from SAB/snapshot, 50-52 and 60-63 context, 64-70 audio, 71+ the guest's)
export const MERGED_ARRAY_SIZE = GUEST_METRICS_BASE + GUEST_METRICS_COUNT;  // 105
