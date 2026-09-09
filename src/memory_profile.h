// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * memory_profile.h — every compile-time size, in one place.
 *
 * Clockwork runs on budgets that differ by four orders of magnitude: a desktop
 * with gigabytes, a WASM worklet with a managed heap, an ESP32-S3 with ~512 KiB
 * of fast internal SRAM behind slower PSRAM. Porting to a new device should mean
 * writing one profile block, not hunting macros across a dozen headers.
 *
 * THREE RULES, each of which something below depends on:
 *
 *   1. This is a PURE-MACRO LEAF. It includes nothing and defines no types, so
 *      the lowest-level headers can pull it in without acquiring dependencies.
 *      Adding an #include here is a cycle waiting to happen.
 *
 *   2. Every knob is `#ifndef`-guarded, which fixes precedence:
 *          -D on the command line  >  the selected device profile  >  defaults
 *
 *   3. The defaults are the desktop / WASM / NIF / test layout. JS reads the
 *      real layout back from the arena header (clockwork_arena.h), so a size
 *      overridden here reaches JavaScript without being restated there.
 *
 * To add a device: give it a CLOCKWORK_PROFILE_* id, add an `#if` block defining
 * only the knobs that differ, and select it with -DCLOCKWORK_DEVICE_PROFILE=<id>.
 */

#ifndef CLOCKWORK_MEMORY_PROFILE_H
#define CLOCKWORK_MEMORY_PROFILE_H

// ── Device profile selection ────────────────────────────────────────────────
// Numeric ids so they can be compared in the preprocessor.
#define CLOCKWORK_PROFILE_DEFAULT 0
#define CLOCKWORK_PROFILE_ESP32S3 1
#define CLOCKWORK_PROFILE_TEENSY41 2

#ifndef CLOCKWORK_DEVICE_PROFILE
#define CLOCKWORK_DEVICE_PROFILE CLOCKWORK_PROFILE_DEFAULT
#endif

// ── ESP32-S3 profile ────────────────────────────────────────────────────────
// Tight budget for the Waveshare ESP32-S3-Touch-LCD-1.9 (ESP32-S3R8: 512 KiB
// internal SRAM, 8 MiB octal PSRAM, 16 MiB flash) + Pimoroni Pico Audio Pack.
//
// Baseline values, not yet retuned against an on-device build — apart from
// CLOCKWORK_HEAP_FAST_SIZE, which was (see its note below). Each is
// `#ifndef`-guarded so a build can still pin any individual knob with an
// explicit -D.
#if CLOCKWORK_DEVICE_PROFILE == CLOCKWORK_PROFILE_ESP32S3

  #ifndef CLOCKWORK_IN_BUFFER_SIZE
  #define CLOCKWORK_IN_BUFFER_SIZE 32768          // 32 KB
  #endif
  #ifndef CLOCKWORK_OUT_BUFFER_SIZE
  #define CLOCKWORK_OUT_BUFFER_SIZE 8192          // 8 KB
  #endif
  #ifndef CLOCKWORK_NRT_OUT_BUFFER_SIZE
  #define CLOCKWORK_NRT_OUT_BUFFER_SIZE 4096        // 4 KB
  #endif
  #ifndef CLOCKWORK_WINDOW_BYTES
  #define CLOCKWORK_WINDOW_BYTES 12304
  #endif
  #ifndef SHM_SCOPE_MAX_SCOPES
  #define SHM_SCOPE_MAX_SCOPES 1
  #endif
  #ifndef SHM_SCOPE_RING_FRAMES
  #define SHM_SCOPE_RING_FRAMES 512
  #endif
  #ifndef SC_MAX_TIMELINES
  #define SC_MAX_TIMELINES 2
  #endif
  #ifndef SCHEDULER_DATA_POOL_SIZE
  #define SCHEDULER_DATA_POOL_SIZE 65536           // 64 KB
  #endif
  #ifndef CLOCKWORK_HEAP_SIZE
  #define CLOCKWORK_HEAP_SIZE 786432              // 768 KB nominal pool budget
  #endif
  #ifndef CLOCKWORK_HEAP_GROWTH_SIZE
  #define CLOCKWORK_HEAP_GROWTH_SIZE 262144       // 256 KB growth (into Bulk/PSRAM)
  #endif
  #ifndef CLOCKWORK_HEAP_FAST_SIZE
  #define CLOCKWORK_HEAP_FAST_SIZE 81920          // 80 KB internal-SRAM initial area. Sized to
                                                    // hold the dsp_new hot working set (audio/
                                                    // control buses, per-sample wire scratch,
                                                    // sample buffers) so the render reads SRAM.
                                                    // Measured on the ESP32-S3: 48 KB overflows
                                                    // into PSRAM growth (heapGrow=1) and the
                                                    // render distorts; 64 KB is the heapGrow=0
                                                    // floor; 80 KB adds headroom for larger
                                                    // scenes.
  #endif
  // Internal SRAM to leave free for allocations made after the engine boots: the
  // audio task's stack (32 KB, internal — it runs the DSP's deep render call chain),
  // I2S DMA descriptors, display/touch drivers. The heap's initial area
  // is bounded by largest_free(Fast) minus this reserve; without it it can claim
  // the whole largest block, and the audio task's xTaskCreate then fails and the
  // engine is never ticked.
  #ifndef CLOCKWORK_FAST_RESERVE
  #define CLOCKWORK_FAST_RESERVE 45056            // 44 KB (32 KB audio task stack + I2S DMA + margin)
  #endif
  // RT heap growth-area size (Bulk/PSRAM) when the Fast initial is exhausted.
  // Non-zero => the pool grows into PSRAM on overflow rather than refusing
  // the allocation the DSP asked for.
  #ifndef CLOCKWORK_RT_POOL_GROWTH_SIZE
  #define CLOCKWORK_RT_POOL_GROWTH_SIZE 65536     // 64 KB
  #endif
  #ifndef CLOCKWORK_MAX_BLOCK_SIZE
  #define CLOCKWORK_MAX_BLOCK_SIZE 64
  #endif
  #ifndef CLOCKWORK_DEFAULT_BLOCK_SIZE
  #define CLOCKWORK_DEFAULT_BLOCK_SIZE 64
  #endif
  #ifndef CLOCKWORK_MAX_CHANNELS
  #define CLOCKWORK_MAX_CHANNELS 2
  #endif
  // The guest's two arenas, when the guest declares no wants of its own
  // (DspInfo::arena_bytes_wanted / arena_bulk_bytes_wanted). Fast comes out of
  // internal SRAM beside the heap's initial area and the audio task's stack, so
  // it is small; bulk comes out of PSRAM. Baselines, not yet retuned on-device.
  #ifndef CLOCKWORK_ARENA_BYTES
  #define CLOCKWORK_ARENA_BYTES (64u * 1024u)             // 64 KB, internal SRAM
  #endif
  #ifndef CLOCKWORK_ARENA_BULK_BYTES
  #define CLOCKWORK_ARENA_BULK_BYTES (2u * 1024u * 1024u) // 2 MB, PSRAM
  #endif
  #ifndef CLOCKWORK_TAP_CHANNELS
  #define CLOCKWORK_TAP_CHANNELS 2
  #endif
  #ifndef CLOCKWORK_SHM_AUDIO_FRAMES
  #define CLOCKWORK_SHM_AUDIO_FRAMES 64
  #endif

  // Sinks: no wide OSC classes on a small SRAM; sysex cells of 4 KB.
  #ifndef CLOCKWORK_OSC_SINK_CLASSES
  #define CLOCKWORK_OSC_SINK_CLASSES "1024:8,8192:2"
  #endif
  #ifndef CLOCKWORK_MIDI_SINK_CLASSES
  #define CLOCKWORK_MIDI_SINK_CLASSES "16:64,4096:2"
  #endif
#endif // CLOCKWORK_PROFILE_ESP32S3

// ── Teensy 4.1 profile ──────────────────────────────────────────────────────
// PJRC Teensy 4.1 (NXP i.MX RT1062: 1 MiB RAM as 512 KiB ITCM/DTCM + 512 KiB
// OCRAM, 8 MiB flash; PSRAM pads unpopulated, so single-tier RAM). The tiered
// knobs (FAST_SIZE/FAST_RESERVE/RT_POOL_GROWTH) stay at their single-region
// defaults. The RT heap is drawn from the OCRAM malloc heap, so
// CLOCKWORK_HEAP_SIZE plus the host's own allocations must fit in ~512 KiB;
// growth is 0 so exhaustion fails the OSC command loudly instead of creeping
// into the audio-library heap. Block size is 128 to match the Teensy audio
// library's per-update frame count (one engine tick per AudioStream update).
// Values retuned against an on-device build.
#if CLOCKWORK_DEVICE_PROFILE == CLOCKWORK_PROFILE_TEENSY41

  #ifndef CLOCKWORK_IN_BUFFER_SIZE
  #define CLOCKWORK_IN_BUFFER_SIZE 32768          // 32 KB
  #endif
  #ifndef CLOCKWORK_OUT_BUFFER_SIZE
  #define CLOCKWORK_OUT_BUFFER_SIZE 8192          // 8 KB
  #endif
  #ifndef CLOCKWORK_NRT_OUT_BUFFER_SIZE
  #define CLOCKWORK_NRT_OUT_BUFFER_SIZE 4096      // 4 KB
  #endif
  #ifndef CLOCKWORK_WINDOW_BYTES
  #define CLOCKWORK_WINDOW_BYTES 12304
  #endif
  #ifndef SHM_SCOPE_MAX_SCOPES
  #define SHM_SCOPE_MAX_SCOPES 1
  #endif
  #ifndef SHM_SCOPE_RING_FRAMES
  #define SHM_SCOPE_RING_FRAMES 512
  #endif
  #ifndef SC_MAX_TIMELINES
  #define SC_MAX_TIMELINES 2
  #endif
  #ifndef SCHEDULER_DATA_POOL_SIZE
  #define SCHEDULER_DATA_POOL_SIZE 65536           // 64 KB
  #endif
  #ifndef CLOCKWORK_HEAP_SIZE
  #define CLOCKWORK_HEAP_SIZE 98304               // 96 KB engine buffer heap (the DSP's buses,
                                                    // sample buffers, wire buffers). OCRAM (~338 KB
                                                    // after the cold .bss) must also hold the
                                                    // guest's memory region and the
                                                    // library's registration mallocs.
  #endif
  #ifndef CLOCKWORK_HEAP_GROWTH_SIZE
  #define CLOCKWORK_HEAP_GROWTH_SIZE 0            // no growth: fail loud, keep OCRAM predictable
  #endif
  #ifndef CLOCKWORK_MAX_BLOCK_SIZE
  #define CLOCKWORK_MAX_BLOCK_SIZE 128
  #endif
  #ifndef CLOCKWORK_DEFAULT_BLOCK_SIZE
  #define CLOCKWORK_DEFAULT_BLOCK_SIZE 128
  #endif
  #ifndef CLOCKWORK_MAX_CHANNELS
  #define CLOCKWORK_MAX_CHANNELS 2
  #endif
  #ifndef CLOCKWORK_TAP_CHANNELS
  #define CLOCKWORK_TAP_CHANNELS 2
  #endif
  #ifndef CLOCKWORK_SHM_AUDIO_FRAMES
  #define CLOCKWORK_SHM_AUDIO_FRAMES 128
  #endif

  // Sinks: no wide OSC classes on a small SRAM; sysex cells of 4 KB.
  #ifndef CLOCKWORK_OSC_SINK_CLASSES
  #define CLOCKWORK_OSC_SINK_CLASSES "1024:8,8192:2"
  #endif
  #ifndef CLOCKWORK_MIDI_SINK_CLASSES
  #define CLOCKWORK_MIDI_SINK_CLASSES "16:64,4096:2"
  #endif
#endif // CLOCKWORK_PROFILE_TEENSY41

// ── Universal defaults (desktop / WASM / NIF / tests) ───────────────────────
// Anything left unset by an explicit -D or by the selected profile falls
// through to here.

// The guest's shared memory region, on a native host with a public segment.
//
// This is where a guest puts bulk it wants a client to see, and where a client
// puts bulk it wants the guest to see. It is NOT in the
// arena: the arena is `ring_buffer_storage`, a static array, and multi-megabyte
// .bss would be paid by every target including the embedded ones. It sits after
// the peer plane in the POSIX segment, which is mmap'd and already bigger than
// the arena.
//
// Present only when there IS a public segment. Headless native has no client to
// share with, so DspConfig::arena stays NULL there and a guest allocates from
// the system as before.
//
// The web equivalent is guestMemorySize in js/memory_layout.js, which is a
// slice of the wasm heap rather than of a segment.
//
// THREE REGIONS, ONE WRITER EACH. The arena is the guest's alone. Inbound bulk
// is written by the client and only read by the guest; outbound bulk is the
// reverse. Splitting them is what removes the need for the two ends to agree at
// runtime about which bytes belong to whom.
#ifndef CLOCKWORK_ARENA_BYTES
#define CLOCKWORK_ARENA_BYTES  (32u * 1024u * 1024u)   // 32 MB, the guest's own
#endif

// The guest's BULK arena — the second kind of RAM, when a device has one.
// DspConfig::arena is the fast tier and DspConfig::arena_bulk this one; a
// single-region platform has no second kind and hands the guest NULL/0 here,
// so the default is 0 and only a tiered profile sets it. The guest's own
// figure (DspInfo::arena_bulk_bytes_wanted) wins over this when it is
// non-zero; this is what a guest that declares nothing gets.
#ifndef CLOCKWORK_ARENA_BULK_BYTES
#define CLOCKWORK_ARENA_BULK_BYTES 0u
#endif

// Bulk staging. Lazily paged like the rest of the segment, so the cost of a
// generous size is address space rather than memory.
#ifndef CLOCKWORK_INBOX_BYTES
#define CLOCKWORK_INBOX_BYTES  (8u * 1024u * 1024u)    // 8 MB, client -> guest
#endif
#ifndef CLOCKWORK_OUTBOX_BYTES
#define CLOCKWORK_OUTBOX_BYTES (8u * 1024u * 1024u)    // 8 MB, guest -> client
#endif

// Event-sink size classes, per kind: "<cell bytes>:<cells>,..." — the queues
// a sink is made of, a message taking the narrowest cell it fits. OSC: sixteen
// ordinary cells, then two of each width up to a whole datagram — 256 KB per
// endpoint. MIDI: every channel and realtime message is three bytes or fewer,
// so 256 tiny cells, and two 64 KB cells for the rare sysex dump (a class is
// two cells at least: the queue needs that to tell full from empty). Parsed
// at boot (sink_profile.h) and installed with clockwork_sink_profile.
#ifndef CLOCKWORK_OSC_SINK_CLASSES
#define CLOCKWORK_OSC_SINK_CLASSES "1024:16,8192:2,16384:2,32768:2,65536:2"
#endif
#ifndef CLOCKWORK_MIDI_SINK_CLASSES
#define CLOCKWORK_MIDI_SINK_CLASSES "16:256,65536:2"
#endif

// SAB / ring-buffer regions
#ifndef CLOCKWORK_IN_BUFFER_SIZE
#define CLOCKWORK_IN_BUFFER_SIZE 786432           // 768 KB
#endif
#ifndef CLOCKWORK_OUT_BUFFER_SIZE
#define CLOCKWORK_OUT_BUFFER_SIZE 131072          // 128 KB
#endif
#ifndef CLOCKWORK_NRT_OUT_BUFFER_SIZE
#define CLOCKWORK_NRT_OUT_BUFFER_SIZE 65536         // 64 KB
#endif
// The window a guest publishes through (DspConfig::shm_window). Clockwork
// reserves it and reads none of it, so the size is the only thing it has an
// opinion about — and that opinion is only "enough for a guest that wants
// one". 96 KB is what a thousand-row structure costs; a guest that publishes
// nothing pays it in address space and never touches the pages.
#ifndef CLOCKWORK_WINDOW_BYTES
#define CLOCKWORK_WINDOW_BYTES 98320
#endif
// The scope-stream slots. SHM_SCOPE_MAX_SCOPES are the GUEST's, in the guest
// region: a client numbers them as it likes and a ScopeOut2 claims one.
// SHM_SCOPE_TRACK_SLOTS are the ENGINE's, in the clockwork block (the
// TRACK_TAPS region, clockwork_arena.h): one per plugin track lane pair
// (plugin_bridge.h LANES / 2), written from the track's return lane
// (src/native/TrackControl.cpp) so a panel can draw what a track is putting
// out without a scope node in the graph. A client numbers the two as one
// index space, the track slots after the guest's (SHM_SCOPE_TRACK_SLOT_BASE).
// No track slots on the web, which hosts no plugins and whose arena is
// budgeted to the byte (js/memory_layout.js), nor on the embedded profiles.
#ifndef SHM_SCOPE_MAX_SCOPES
#define SHM_SCOPE_MAX_SCOPES 32
#endif
#ifndef SHM_SCOPE_TRACK_SLOTS
  #if defined(__EMSCRIPTEN__)
  #define SHM_SCOPE_TRACK_SLOTS 0
  #else
  #define SHM_SCOPE_TRACK_SLOTS 16
  #endif
#endif
#define SHM_SCOPE_TRACK_SLOT_BASE SHM_SCOPE_MAX_SCOPES
// Per-slot scope stream ring, in frames. Sized for the longest display window
// a consumer draws (a client's inline scopes scroll ~250ms) plus output
// latency and reader slack: 16384 ≈ 340ms @ 48k, 128KB per stereo slot
// (4 MB for the guest's 32, and 2 MB more for the 16 track taps natively).
#ifndef SHM_SCOPE_RING_FRAMES
#define SHM_SCOPE_RING_FRAMES 16384
#endif

// Max MIDI-clock follower timelines in the ClockworkClock registry (slot 0 is
// always Link; slots 1..SC_MAX_TIMELINES are midi:<port> followers).
#ifndef SC_MAX_TIMELINES
#define SC_MAX_TIMELINES 8
#endif

// Scheduler pool
#ifndef SCHEDULER_DATA_POOL_SIZE
#define SCHEDULER_DATA_POOL_SIZE (512 * 1024)      // 512 KB
#endif
#ifndef SCHEDULER_SLOT_COUNT
#define SCHEDULER_SLOT_COUNT 512
#endif

// The buffer heap on WASM: modest pool, one spare growth area. (Desktop keeps
// the 64 MB default below; the ESP32 profile sets its own.)
#if defined(__EMSCRIPTEN__)
  #ifndef CLOCKWORK_HEAP_SIZE
  #define CLOCKWORK_HEAP_SIZE (8 * 1024 * 1024)
  #endif
  #ifndef CLOCKWORK_HEAP_GROWTH_SIZE
  #define CLOCKWORK_HEAP_GROWTH_SIZE (4 * 1024 * 1024)
  #endif
#endif

// Fire-time load shedding: a scheduled event already more than this many ms
// past its timetag when it comes due is dropped (counted in the dropped
// metric, logged rate-limited) instead of played. 0 = never shed — classic
// semantics, and the default everywhere compat is measured (native,
// NRT). The web build sets a threshold: an interactive engine that falls
// behind should shed the unplayable backlog, not perform it minutes late —
// capacity without shedding converts overload from drops into unbounded
// lateness (measured: a mashed bench snowballed to 4.5 s).
#ifndef SCHEDULER_SHED_LATE_MS
#define SCHEDULER_SHED_LATE_MS 0
#endif

// A RUST-SIDE ENGINE ARENA WAS DECLARED HERE and never wired up.
//
// CLOCKWORK_RUST_ARENA_SIZE (12 MB) had no reader anywhere in the tree, and the
// mechanism its comment described is not built: rust/clockwork-heap's RtRouter
// exists but is never installed as #[global_allocator] and router::arm is
// never called outside its own tests. The comment also still sized itself
// against a "32 MB reserved RT region" that js/memory_layout.js replaced with
// an opaque guest region. A constant nobody reads, describing a mechanism
// nobody installed, against a layout that no longer exists.
//
// If the router is ever installed it will need a size again — and it should be
// measured then rather than inherited from this one, which was a census of one
// particular guest.

// RT heap (rust/clockwork-heap). Read by clockwork_heap_init via CLOCKWORK_HEAP_FAST_SIZE below,
// which is what actually sizes the pool — on a profile that overrides
// FAST_SIZE (the ESP32-S3 one does) this figure sizes nothing, so change the
// two together or the one you changed will be the one that does not matter.
#ifndef CLOCKWORK_HEAP_SIZE
#define CLOCKWORK_HEAP_SIZE (64 * 1024 * 1024)    // 64 MB
#endif
#ifndef CLOCKWORK_HEAP_GROWTH_SIZE
#define CLOCKWORK_HEAP_GROWTH_SIZE (16 * 1024 * 1024) // 16 MB growth area when exhausted
#endif
// The initial pool area drawn from the Fast tier (mem_region.h). On desktop/WASM
// this equals CLOCKWORK_HEAP_SIZE, so the pool is one region and behaviour is
// unchanged; an embedded profile shrinks it so only the boot-time hot set lands
// in fast internal SRAM and later/large buffers grow into Bulk (PSRAM).
#ifndef CLOCKWORK_HEAP_FAST_SIZE
#define CLOCKWORK_HEAP_FAST_SIZE CLOCKWORK_HEAP_SIZE
#endif
// Fast-tier reserve + heap growth. Both 0 on single-tier targets: largest_free()
// returns SIZE_MAX there so the boot-time bounding never clamps, and growth 0 keeps
// the heap a fixed region with no allocation after boot (the desktop/NIF shape).
// An embedded profile sets both non-zero.
#ifndef CLOCKWORK_FAST_RESERVE
#define CLOCKWORK_FAST_RESERVE 0
#endif
#ifndef CLOCKWORK_RT_POOL_GROWTH_SIZE
#define CLOCKWORK_RT_POOL_GROWTH_SIZE 0
#endif

// Audio graph caps (non-WASM; the WASM render quantum is fixed at 128)
#ifndef CLOCKWORK_MAX_BLOCK_SIZE
#define CLOCKWORK_MAX_BLOCK_SIZE 1024
#endif
#ifndef CLOCKWORK_DEFAULT_BLOCK_SIZE
#define CLOCKWORK_DEFAULT_BLOCK_SIZE 128
#endif
#ifndef CLOCKWORK_MAX_CHANNELS
#define CLOCKWORK_MAX_CHANNELS 128
#endif

// The audio taps (shm_audio_buffer.hpp): two slots, out and in, written by
// clockwork at the device edge. Each slot's ring is sized for this many
// channels and carries the device's live count up to it: a stereo interface
// fills two, an eight-out interface fills eight, anything wider is tapped to
// the ceiling. Two on the web, whose arena is budgeted to the byte and whose
// output is stereo; eight on the desktop, at 1.5 MB per slot per second.
#ifndef CLOCKWORK_TAP_CHANNELS
  #if defined(__EMSCRIPTEN__)
  #define CLOCKWORK_TAP_CHANNELS 2
  #else
  #define CLOCKWORK_TAP_CHANNELS 8
  #endif
#endif
#ifndef CLOCKWORK_SHM_AUDIO_SECONDS
#define CLOCKWORK_SHM_AUDIO_SECONDS 1
#endif
#ifndef CLOCKWORK_SHM_AUDIO_SAMPLE_RATE
#define CLOCKWORK_SHM_AUDIO_SAMPLE_RATE 48000
#endif
// Per-slot frames default to seconds * sample-rate; a profile may override the
// frame count directly (e.g. to shrink below one second, which the seconds knob
// can't express because it is an integer).
#ifndef CLOCKWORK_SHM_AUDIO_FRAMES
#define CLOCKWORK_SHM_AUDIO_FRAMES \
    (CLOCKWORK_SHM_AUDIO_SAMPLE_RATE * CLOCKWORK_SHM_AUDIO_SECONDS)
#endif

#endif // CLOCKWORK_MEMORY_PROFILE_H
