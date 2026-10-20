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
// CLOCKWORK_GUEST_MEMORY_FAST_BYTES, which was (see its note below). Each is
// `#ifndef`-guarded so a build can still pin any individual knob with an
// explicit -D.
#if CLOCKWORK_DEVICE_PROFILE == CLOCKWORK_PROFILE_ESP32S3

  #ifndef CLOCKWORK_IN_RING_SIZE
  #define CLOCKWORK_IN_RING_SIZE 32768          // 32 KB
  #endif
  #ifndef CLOCKWORK_RT_OUT_RING_SIZE
  #define CLOCKWORK_RT_OUT_RING_SIZE 8192          // 8 KB
  #endif
  #ifndef CLOCKWORK_NRT_OUT_RING_SIZE
  #define CLOCKWORK_NRT_OUT_RING_SIZE 4096        // 4 KB
  #endif
  // The arena lives in internal SRAM (audio_processor.cpp: its hot regions
  // are touched every block), so its bulk-tier regions are kept small here:
  // no track-tap slots (no plugin tracks on a device), a 4 KB persistent
  // store. Baselines, not yet measured on-device.
  #ifndef TAP_TRACK_SLOTS
  #define TAP_TRACK_SLOTS 0
  #endif
  #ifndef GUEST_PERSIST_BYTES
  #define GUEST_PERSIST_BYTES (4u * 1024u)
  #endif
  #ifndef CLOCKWORK_WINDOW_BYTES
  #define CLOCKWORK_WINDOW_BYTES 12304
  #endif
  #ifndef TAPS_SLOTS
  #define TAPS_SLOTS 1
  #endif
  #ifndef TAP_RING_FRAMES
  #define TAP_RING_FRAMES 512
  #endif
  #ifndef CLOCKWORK_MAX_TIMELINES
  #define CLOCKWORK_MAX_TIMELINES 2
  #endif
  #ifndef SCHEDULER_DATA_POOL_SIZE
  #define SCHEDULER_DATA_POOL_SIZE 65536           // 64 KB
  #endif
  #ifndef CLOCKWORK_ROUTER_BYTES
  #define CLOCKWORK_ROUTER_BYTES 65536            // 64 KB, the least the router arms on: the sink and port queues
  #endif
  // The guest's fast working memory when the guest declares no fast want of
  // its own: 80 KB of internal SRAM. Sized to hold a server guest's dsp_new
  // hot working set (audio/control buses, per-sample wire scratch) so the
  // render reads SRAM. Measured on the ESP32-S3 (as the heap's fast area,
  // which this replaces): 48 KB overflows into PSRAM and the render distorts;
  // 64 KB is the floor; 80 KB adds headroom for larger scenes. A guest's own
  // figure (DspInfo::memory_bytes_wanted, dsp_memory_bytes) wins over this.
  #ifndef CLOCKWORK_GUEST_MEMORY_FAST_BYTES
  #define CLOCKWORK_GUEST_MEMORY_FAST_BYTES 81920
  #endif
  // Internal SRAM to leave free for allocations made after the engine boots: the
  // audio task's stack (32 KB, internal — it runs the DSP's deep render call chain),
  // I2S DMA descriptors, display/touch drivers. The profile's default fast
  // span above is bounded by largest_free(Fast) minus this reserve; without
  // it it can claim the whole largest block, and the audio task's
  // xTaskCreate then fails and the engine is never ticked. (A guest's own
  // want is never clamped: it is met or the boot is refused.)
  #ifndef CLOCKWORK_FAST_RESERVE
  #define CLOCKWORK_FAST_RESERVE 45056            // 44 KB (32 KB audio task stack + I2S DMA + margin)
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
  // The guest's bulk memory — the span its pool runs over — when the guest
  // declares no bulk want of its own (DspInfo::memory_bulk_bytes_wanted,
  // dsp_memory_bulk_bytes): 2 MB of PSRAM. A baseline, not yet retuned
  // on-device.
  #ifndef CLOCKWORK_GUEST_MEMORY_BULK_BYTES
  #define CLOCKWORK_GUEST_MEMORY_BULK_BYTES (2u * 1024u * 1024u) // 2 MB, PSRAM
  #endif
  #ifndef CLOCKWORK_TAP_CHANNELS
  #define CLOCKWORK_TAP_CHANNELS 2
  #endif
  #ifndef CLOCKWORK_SCOPE_FRAMES
  #define CLOCKWORK_SCOPE_FRAMES 64
  #endif
  // Committed assets the guest may hold at once (the asset table): a
  // sample buffer count on the scale of an embedded guest's.
  #ifndef CLOCKWORK_ASSET_SLOTS
  #define CLOCKWORK_ASSET_SLOTS 1024
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
// knobs (FAST_RESERVE, the fast default) stay at their single-region
// defaults. The guest's bulk memory is drawn from the OCRAM malloc heap, so
// CLOCKWORK_GUEST_MEMORY_BULK_BYTES plus the host's own allocations must fit
// in ~512 KiB; the pool never grows, so exhaustion fails the OSC command
// loudly instead of creeping into the audio-library heap. Block size is 128 to
// match the Teensy audio library's per-update frame count (one engine tick
// per AudioStream update). Values retuned against an on-device build.
#if CLOCKWORK_DEVICE_PROFILE == CLOCKWORK_PROFILE_TEENSY41

  // As the ESP32-S3 profile: the arena is one span in fast RAM, so its
  // bulk-tier regions are kept small.
  #ifndef TAP_TRACK_SLOTS
  #define TAP_TRACK_SLOTS 0
  #endif
  #ifndef GUEST_PERSIST_BYTES
  #define GUEST_PERSIST_BYTES (4u * 1024u)
  #endif
  #ifndef CLOCKWORK_IN_RING_SIZE
  #define CLOCKWORK_IN_RING_SIZE 32768          // 32 KB
  #endif
  #ifndef CLOCKWORK_RT_OUT_RING_SIZE
  #define CLOCKWORK_RT_OUT_RING_SIZE 8192          // 8 KB
  #endif
  #ifndef CLOCKWORK_NRT_OUT_RING_SIZE
  #define CLOCKWORK_NRT_OUT_RING_SIZE 4096      // 4 KB
  #endif
  #ifndef CLOCKWORK_WINDOW_BYTES
  #define CLOCKWORK_WINDOW_BYTES 12304
  #endif
  #ifndef TAPS_SLOTS
  #define TAPS_SLOTS 1
  #endif
  #ifndef TAP_RING_FRAMES
  #define TAP_RING_FRAMES 512
  #endif
  #ifndef CLOCKWORK_MAX_TIMELINES
  #define CLOCKWORK_MAX_TIMELINES 2
  #endif
  #ifndef SCHEDULER_DATA_POOL_SIZE
  #define SCHEDULER_DATA_POOL_SIZE 65536           // 64 KB
  #endif
  #ifndef CLOCKWORK_GUEST_MEMORY_BULK_BYTES
  #define CLOCKWORK_GUEST_MEMORY_BULK_BYTES 98304 // 96 KB: the guest's bulk memory (sample buffers,
                                                    // definitions). OCRAM (~338 KB after the cold
                                                    // .bss) must also hold the guest's fast memory
                                                    // and the library's registration mallocs.
  #endif
  #ifndef CLOCKWORK_ROUTER_BYTES
  #define CLOCKWORK_ROUTER_BYTES 0                // no router block: OCRAM has no 64 KB to spare, so the Rust
                                                    // subsystems' queues stay with the library's malloc
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
  #ifndef CLOCKWORK_SCOPE_FRAMES
  #define CLOCKWORK_SCOPE_FRAMES 128
  #endif
  // Committed assets the guest may hold at once (the asset table): a
  // sample buffer count on the scale of an embedded guest's.
  #ifndef CLOCKWORK_ASSET_SLOTS
  #define CLOCKWORK_ASSET_SLOTS 1024
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

// THE GUEST'S MEMORY: two spans, one per speed (dsp_api.h, DspConfig::memory
// and memory_bulk). Each is sized as the larger of what the guest declares
// (DspInfo::memory_bytes_wanted / memory_bulk_bytes_wanted, plus what its
// configuration adds through dsp_memory_bytes / dsp_memory_bulk_bytes) and
// the profile's default here. A guest's own figure is met or the boot is
// refused; the default is clamped to what the tier has. Neither span is in
// the arena and no client can reach them.
//
// FAST: what the guest lays out by hand, its hot per-block state. The
// default is 0 — a guest knows its own hot state and says so — and only a
// tiered profile gives a floor (the ESP32's 80 KB, above).
#ifndef CLOCKWORK_GUEST_MEMORY_FAST_BYTES
#define CLOCKWORK_GUEST_MEMORY_FAST_BYTES 0u
#endif
// BULK: the span the guest's pool runs over (clockwork_pool.h) — everything
// it allocates as it runs. 64 MB on the desktop and the NIF, where it used
// to be clockwork's own heap of the same size. On the web the figure is a
// ceiling and the span takes what is left of the engine's memory after the
// fast span and the RT router's block (less CLOCKWORK_FAST_RESERVE, the
// arena's own headers), which is what the client sized that memory for
// (memArenaSize in js/memory_layout.js). The embedded profiles set their
// own, above. The pool never grows: a guest that needs more declares more.
#ifndef CLOCKWORK_GUEST_MEMORY_BULK_BYTES
  #if defined(__EMSCRIPTEN__)
  #define CLOCKWORK_GUEST_MEMORY_BULK_BYTES (2048u * 1024u * 1024u)   // a ceiling; the engine's memory bounds it
  #else
  #define CLOCKWORK_GUEST_MEMORY_BULK_BYTES (64u * 1024u * 1024u)     // 64 MB
  #endif
#endif

// Bulk staging. Lazily paged like the rest of the segment, so the cost of a
// generous size is address space rather than memory.
#ifndef CLOCKWORK_ASSETS_BYTES
#define CLOCKWORK_ASSETS_BYTES  (8u * 1024u * 1024u)    // 8 MB, client -> guest
#endif
#ifndef CLOCKWORK_RETURNS_BYTES
#define CLOCKWORK_RETURNS_BYTES (8u * 1024u * 1024u)    // 8 MB, guest -> client
#endif

// Committed assets the guest may hold at once: the asset table, one entry
// per asset, kept across a rebuild so a new instance is handed
// every one (docs/RESTARTS.md). A power of two; a commit past what the
// table holds is refused.
#ifndef CLOCKWORK_ASSET_SLOTS
#define CLOCKWORK_ASSET_SLOTS 4096
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
#ifndef CLOCKWORK_IN_RING_SIZE
#define CLOCKWORK_IN_RING_SIZE 786432           // 768 KB
#endif
#ifndef CLOCKWORK_RT_OUT_RING_SIZE
#define CLOCKWORK_RT_OUT_RING_SIZE 131072          // 128 KB
#endif
#ifndef CLOCKWORK_NRT_OUT_RING_SIZE
#define CLOCKWORK_NRT_OUT_RING_SIZE 65536         // 64 KB
#endif
// The window a guest publishes through (DspConfig::shm_window). Clockwork
// reserves it and reads none of it, so the size is the only thing it has an
// opinion about — and that opinion is only "enough for a guest that wants
// one". 96 KB is what a thousand-row structure costs; a guest that publishes
// nothing pays it in address space and never touches the pages.
#ifndef CLOCKWORK_WINDOW_BYTES
#define CLOCKWORK_WINDOW_BYTES 98320
#endif
// The tap-stream slots. TAPS_SLOTS are the GUEST's, in the guest
// region: a client numbers them as it likes and a ScopeOut2 claims one.
// TAP_TRACK_SLOTS are the ENGINE's, after the guest's slots in the one tap
// region (clockwork_arena.h): one per plugin track lane pair
// (plugin_bridge.h LANES / 2), written from the track's return lane
// (src/native/TrackControl.cpp) so a panel can draw what a track is putting
// out without a scope node in the graph. A client numbers the two as one
// index space, the track slots after the guest's (TAP_TRACK_SLOT_BASE).
// No track slots on the web, which hosts no plugins and whose arena is
// budgeted to the byte (js/memory_layout.js), nor on the embedded profiles.
#ifndef TAPS_SLOTS
#define TAPS_SLOTS 32
#endif
#ifndef TAP_TRACK_SLOTS
  #if defined(__EMSCRIPTEN__)
  #define TAP_TRACK_SLOTS 0
  #else
  #define TAP_TRACK_SLOTS 16
  #endif
#endif
#define TAP_TRACK_SLOT_BASE TAPS_SLOTS
// Per-slot tap stream ring, in frames. A consumer draws the window being
// HEARD, which sits the device's output latency behind the writer, so the
// ring must hold that latency plus the longest display window (a client's
// inline scopes scroll ~250ms) plus the reader's margin — the rule is
// shm_tap_ring_covers (shm_tap_stream.hpp), checked at every device
// start. Natively a wireless output can report two seconds (an AirPlay
// receiver: 88712 frames @ 44.1k, 2026-10-06; a 16384 ring had every scope
// flat while the engine rendered): 131072 ≈ 2.7s @ 48k, 1 MB per stereo
// slot, and the segment is anonymous memory committed only as a slot is
// written, so idle slots cost nothing. The web renders into a worklet with
// tens of ms of latency and budgets its arena to the byte
// (js/memory_layout.js): 16384 ≈ 340ms @ 48k, 128KB per slot, 4 MB for the
// guest's 32.
#ifndef TAP_RING_FRAMES
  #if defined(__EMSCRIPTEN__)
  #define TAP_RING_FRAMES 16384
  #else
  #define TAP_RING_FRAMES 131072
  #endif
#endif

// Max MIDI-clock follower timelines in the ClockworkClock registry (slot 0 is
// always Link; slots 1..CLOCKWORK_MAX_TIMELINES are midi:<port> followers).
#ifndef CLOCKWORK_MAX_TIMELINES
#define CLOCKWORK_MAX_TIMELINES 8
#endif

// Scheduler pool
#ifndef SCHEDULER_DATA_POOL_SIZE
#define SCHEDULER_DATA_POOL_SIZE (512 * 1024)      // 512 KB
#endif
#ifndef SCHEDULER_SLOT_COUNT
#define SCHEDULER_SLOT_COUNT 512
#endif

// What the guest's bulk span leaves of the engine's memory on the web: the
// arena's own block headers. The RT router's block and the guest's fast
// span are taken before the bulk span is sized, so they are already out of
// what it sees.
#if defined(__EMSCRIPTEN__)
  #ifndef CLOCKWORK_FAST_RESERVE
  #define CLOCKWORK_FAST_RESERVE (64 * 1024)
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

// The RT router's block (rust/clockwork-heap/src/router.rs): what clockwork's
// Rust subsystems allocate inside an engine entry point — a sink's or a port's
// queues, opened from the audio thread — comes from this one block, taken from
// the Fast tier once at boot and kept for the life of the process. The
// router arms on nothing under 64 KB; 0 leaves it unarmed, and those
// allocations stay with the system allocator.
#ifndef CLOCKWORK_ROUTER_BYTES
#define CLOCKWORK_ROUTER_BYTES (1 * 1024 * 1024)      // 1 MB
#endif
// Fast-tier reserve: what a profile's DEFAULT span leaves of the tier's
// largest free block. 0 on desktop and the NIF: largest_free() returns
// SIZE_MAX there so the bounding never clamps. The embedded profile and the
// web set their own above.
#ifndef CLOCKWORK_FAST_RESERVE
#define CLOCKWORK_FAST_RESERVE 0
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

// The scope (shm_audio_buffer.hpp): two slots, out and in, written by
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
#ifndef CLOCKWORK_SCOPE_SECONDS
#define CLOCKWORK_SCOPE_SECONDS 1
#endif
#ifndef CLOCKWORK_SCOPE_SAMPLE_RATE
#define CLOCKWORK_SCOPE_SAMPLE_RATE 48000
#endif
// Per-slot frames default to seconds * sample-rate; a profile may override the
// frame count directly (e.g. to shrink below one second, which the seconds knob
// can't express because it is an integer).
#ifndef CLOCKWORK_SCOPE_FRAMES
#define CLOCKWORK_SCOPE_FRAMES \
    (CLOCKWORK_SCOPE_SAMPLE_RATE * CLOCKWORK_SCOPE_SECONDS)
#endif

#endif // CLOCKWORK_MEMORY_PROFILE_H
