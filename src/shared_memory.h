// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#ifndef CLOCKWORK_SHARED_MEMORY_H
#define CLOCKWORK_SHARED_MEMORY_H

#include "clock/clock_math.h"   // beatAt, for ClockworkClockSnapshot
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "memory_profile.h"
#include "clockwork_arena.h"   // the table every reader takes the layout from
#include "ring/ring.h"   // Message, MESSAGE_MAGIC, PADDING_MAGIC
#include "shm_audio_buffer.hpp"
#include "shm_scope_stream.hpp"

namespace clockwork {

// Double ↔ uint64 bit-pattern conversion. Centralised so the C++ mirror of
// ClockworkClockState (and any future SAB struct storing a double in a 64-bit
// atomic) has one bit-cast spelling.
//
// __builtin_bit_cast, not memcpy: ESP-IDF compiles every translation unit with
// -fno-builtin-memcpy, so an eight-byte memcpy here does not fold into a pair
// of loads — it emits a call to the library memcpy, on the audio thread, once a
// block through TimeSource::nowAt and again in every clock publish. The builtin
// has no such escape hatch, and it is the same
// reinterpretation memcpy performed, spelled so the compiler must do it in
// line. The memcpy form stays behind it so a compiler without the builtin
// (which includes emcc's C++17 default, where std::bit_cast is not available
// either) still builds.
#if defined(__has_builtin)
#  if __has_builtin(__builtin_bit_cast)
#    define CLOCKWORK_BIT_CAST_BUILTIN 1
#  endif
#endif

inline uint64_t doubleToBits(double v) {
#ifdef CLOCKWORK_BIT_CAST_BUILTIN
    return __builtin_bit_cast(uint64_t, v);
#else
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(double));
    return bits;
#endif
}

inline double bitsToDouble(uint64_t bits) {
#ifdef CLOCKWORK_BIT_CAST_BUILTIN
    return __builtin_bit_cast(double, bits);
#else
    double v;
    std::memcpy(&v, &bits, sizeof(double));
    return v;
#endif
}

// Session tempo a fresh boot opens at, before any peer, local set, or embedder
// override. Matches Ableton Link's own default; single source for the SAB seed,
// the Link session ctor, and Config::defaultBpm. An embedder
// overrides it at construction via Config::defaultBpm.
constexpr double kDefaultBpm = 120.0;

}  // namespace clockwork

// ============================================================================
// BUFFER LAYOUT CONFIGURATION
// ============================================================================
// Memory layout constants for shared ring buffer.
// NOTE: These are RELATIVE OFFSETS from the ring buffer base address.
// Actual addresses are calculated as: base_address + offset
// On WASM the base is the linker's static buffer (data segment), retrieved via
// get_ring_buffer_base() at runtime; every other runtime takes it from
// get_shared_memory_base() — the local arena, or an external segment when one
// was supplied.
//
// TO MODIFY BUFFER SIZES: Change the SIZE constants below.
// All offsets are calculated automatically using constexpr.
// ============================================================================

// User-configurable buffer sizes.
// Balanced to prevent message drops in the audioworklet; sized per device via
// memory_profile.h (defaults: IN 768KB, OUT 128KB, NRT-out 64KB).
constexpr uint32_t IN_BUFFER_SIZE     = CLOCKWORK_IN_BUFFER_SIZE;    // OSC messages from host to the engine (large for bulk payloads)
constexpr uint32_t OUT_BUFFER_SIZE    = CLOCKWORK_OUT_BUFFER_SIZE;   // OSC replies from the engine to host (prevent drops)
constexpr uint32_t NRT_OUT_BUFFER_SIZE  = CLOCKWORK_NRT_OUT_BUFFER_SIZE; // NRT-thread egress ring (replies, notifications, debug)
constexpr uint32_t CONTROL_SIZE       = 48;    // Atomic control pointers & flags (11 fields × 4 bytes + 4 padding for 8-byte alignment)
constexpr uint32_t METRICS_SIZE       = 208;   // Performance metrics: 52 fields * 4 bytes = 208 bytes (multiple of 8)
constexpr uint32_t NTP_START_TIME_SIZE = 8;    // NTP time when AudioContext started (double, 8-byte aligned, write-once)
constexpr uint32_t DRIFT_OFFSET_SIZE = 4;      // Drift offset in microseconds (int32, atomic)
constexpr uint32_t GLOBAL_OFFSET_SIZE = 4;     // Global timing offset in milliseconds (int32, atomic) - for multi-system sync (Ableton Link, NTP, etc.)
constexpr uint32_t CLOCK_STATE_SIZE = 40; // ClockworkClock session state: 3 atomic uint64 + 3 atomic uint32 + 4 padding = 40 bytes

// Node tree mirror configuration (for observing synth/group hierarchy via polling)
// A window of shared memory the guest may publish through, reached as
// DspConfig::shm_window. Clockwork reserves it, hands over base and length,
// and never reads a byte: what goes in it, and in what shape, is the guest's.
//
// Sized per device via memory_profile.h. A guest that publishes nothing simply
// leaves it alone.
constexpr uint32_t SHM_WINDOW_SIZE = CLOCKWORK_WINDOW_BYTES;

// The audio taps: two shm_audio_buffer slots, out and in, written by
// clockwork at the device edge inside every tick on every host
// (shm_audio_buffer.hpp). Per-slot ring duration is CLOCKWORK_SHM_AUDIO_SECONDS
// (1s in production, larger in test builds so captures don't wrap).

// Where each region sits is decided in one place — "The arena map" below —
// once every size is known.
constexpr uint32_t NODE_ID_COUNTER_SIZE  = 4;     // Int32, atomic — for nextNodeId() range allocation

// Guest config — bytes written by the HOST before the guest boots and read by
// the GUEST, with clockwork in between carrying them and nothing more. It
// copies whatever it is handed into this region (clockwork_init) and passes base and
// length on as DspConfig::guest_config. No clockwork code reads a field, so the
// layout is a private matter between a host and its guest: this repository's
// native host writes src/native/GuestConfigBlock.h's, and the web one writes
// whatever the product's encodeGuestConfig() returns.
//
// The size is a ceiling, not a shape. A block that does not fit is refused
// loudly on both sides rather than truncated, because half a config block is a
// different config and the guest cannot tell.
constexpr uint32_t GUEST_CONFIG_SIZE  = 32 * sizeof(uint32_t);  // 128 bytes

// Scope streams — per-slot lossless audio rings for visualisation.
// Protocol (struct layout, writer/reader) lives in
// shm_scope_stream.hpp; rationale in docs/PORTS.md.
// The stream's producer appends every block; readers combine a slot's cursor with the
// sample-clock region (below) and their own wall clock to display exactly what
// the listener is hearing. SHM_SCOPE_MAX_SCOPES / SHM_SCOPE_RING_FRAMES sized
// per device via memory_profile.h: 32 slots for the guest here, in the guest
// region; the 16 track-tap slots the ENGINE writes natively (TrackControl.cpp)
// are the TRACK_TAPS region in the clockwork block, numbered after these in
// one index space (scopeSlotAt below); 16384 frames per slot.
constexpr uint32_t SHM_SCOPE_CHANNELS = SHM_SCOPE_STREAM_CHANNELS;
constexpr uint32_t SHM_SCOPE_HEADER_SIZE = 32;  // Global header (16-aligned)
constexpr uint32_t SHM_SCOPE_SLOT_HEADER_SIZE = SHM_SCOPE_STREAM_HEADER_SIZE;
constexpr uint32_t SHM_SCOPE_SLOT_SIZE = SHM_SCOPE_STREAM_SLOT_SIZE;
constexpr uint32_t SHM_SCOPE_TOTAL_SIZE = SHM_SCOPE_HEADER_SIZE + (SHM_SCOPE_MAX_SCOPES * SHM_SCOPE_SLOT_SIZE);

// Track taps: the engine's scope-stream slots (memory_profile.h), the same
// slot shape as the guest's, no global header — the arena table carries the
// geometry. Slot index SHM_SCOPE_TRACK_SLOT_BASE + i lives here.
constexpr uint32_t SHM_TRACK_TAPS_SLOTS = SHM_SCOPE_TRACK_SLOTS;
constexpr uint32_t SHM_TRACK_TAPS_SIZE  = SHM_TRACK_TAPS_SLOTS * SHM_SCOPE_SLOT_SIZE;

// Scope header layout (at SHM_SCOPE_START):
//   [0..3]   u32  maxScopes
//   [4..7]   u32  activeCount
//   [8..11]  u32  ringFrames (per-slot capacity)
//   [12..15] u32  version (increments on scope add/remove)
//   [16..31] reserved
//
// Per-scope slot: a shm_scope_stream (see shm_scope_stream.hpp).

// Native-only live engine stats. Kept apart from PerformanceMetrics so that
// struct stays a clean cross-platform surface rather than a pile of fields
// that are 0 on half the runtimes (the web build never writes this region —
// it has its own JS-context equivalents). The native engine writes it; an
// observer finds it through the arena table.
constexpr uint32_t NATIVE_STATS_SIZE  = 24;  // u32 x6 (see field offsets below)
// Field byte offsets within the native-stats region.
constexpr uint32_t NATIVE_STAT_CPU_AVG_CENTI  = 0;   // DSP load, percent * 100 (smoothed average)
constexpr uint32_t NATIVE_STAT_CPU_PEAK_CENTI = 4;   // DSP load, percent * 100 (decaying peak)
constexpr uint32_t NATIVE_STAT_CB_OVERRUNS    = 8;   // audio callbacks that overran their time budget
// NRT control-thread blocking, MICROSECONDS (healthy passes are µs-scale, so
// integer ms truncates every reading to a meaningless 0). That thread is the
// sole non-RT consumer, so a handler which blocks stalls every control command
// and reply queued behind it while the socket stays up and the audio keeps
// ticking.
// MAX is the high-water mark; IN_FLIGHT is non-zero only while a pass is
// running long right now — a stall in progress, which a high-water mark cannot
// show until it ends.
constexpr uint32_t NATIVE_STAT_NRT_MAX_PASS_US  = 12;
constexpr uint32_t NATIVE_STAT_NRT_IN_FLIGHT_US = 16;
// Worst pass completed in the trailing ~60 s window: unlike MAX it decays back
// to quiet, so it distinguishes "struggling now" from "hiccuped hours ago".
constexpr uint32_t NATIVE_STAT_NRT_RECENT_WORST_US = 20;

// ClockworkClock's sample clock — the engine's sample position anchored to
// wall-clock DAC time. One anchor plus the rate defines the whole line
//   dac_time(frame) = dac_ntp + (frame - engine_frames) / sample_rate
// so any reader can compute e.g. "which sample is the listener hearing":
//   visible = engine_frames + (now_ntp - dac_ntp) * sample_rate
// Published once per hardware callback via ClockworkClock::publishSampleClock
// (seqlock: odd SEQ, fields, even SEQ with release ordering). Consumers:
// scope streams, plus anything needing audible-time alignment (recording
// markers, visual sync). See docs/PORTS.md.
constexpr uint32_t SAMPLE_CLOCK_SIZE  = 32;
// Field byte offsets within the sample-clock region.
constexpr uint32_t SAMPLE_CLOCK_SEQ            = 0;   // u32 seqlock (odd = mid-update)
constexpr uint32_t SAMPLE_CLOCK_SAMPLE_RATE    = 4;   // u32
constexpr uint32_t SAMPLE_CLOCK_ENGINE_FRAMES  = 8;   // u64 engine frames at block start
constexpr uint32_t SAMPLE_CLOCK_DAC_NTP        = 16;  // f64 NTP seconds when that frame hits the DAC
constexpr uint32_t SAMPLE_CLOCK_OUT_LATENCY    = 24;  // u32 device output latency, frames
                                                  // [28..31] reserved

// ─── Guest persistence ──────────────────────────────────────────────────────
//
// PRIVATE to the DSP, and it OUTLIVES THE DSP. That second half is the whole
// point and is why this is a region rather than a callback.
//
// A DSP instance is destroyed and rebuilt on a device switch, and in the
// browser it can also simply be killed — the worklet is torn down when a tab is
// backgrounded or the renderer runs short of memory. Nothing runs at that
// moment. A save/restore hook cannot help, because there is no thread left to
// call it on. Whatever must come back has to have been written down already.
//
// So the host guarantees only this: these bytes are not touched by clockwork,
// not zeroed between DSP instances (a cold boot does clear the region; a rebuild
// does not), and are still here when the next dsp_new runs.
// They live in the arena, which the host owns and which outlives any guest.
//
// WHAT THE GUEST OWES IN RETURN. This is hostile memory and the contract says
// so rather than pretending otherwise:
//
//   - It may be TORN. A worklet that died mid-write left half an update here.
//     Stamp a generation and validate it; do not trust a struct because its
//     pointer is non-null. `readClockworkClock` below shows the house style for a
//     directional protocol.
//   - It may be STALE, from a run that crashed. Restoring blindly can resurrect
//     whatever caused the crash, so a guest should be able to decline.
//   - It is NOT durable. Shared memory, not a file: it does not survive the
//     process, which is what dsp_save/dsp_load would be for if the state ever
//     needs to reach a DAW project file or a postMessage worklet, where there
//     is no shared memory at all.
//
// Deliberately NOT the same bytes as DspConfig::shm_window. That window is
// PUBLISHED — clients poll it, the node-tree mirror lives there, and it is
// rebuilt freely. Conflating "state a client may read" with "state that must
// survive me" gives one region two lifetimes and two tearing rules. This one is
// the guest's alone; nothing outside it may read or write these bytes.
//
// Sized modestly and overridable with -DGUEST_PERSIST_BYTES=N: an ESP32-S3 has
// 512 KiB of SRAM and should not spend 64 of it on a region it may not use.
#ifndef GUEST_PERSIST_BYTES
#define GUEST_PERSIST_BYTES (64u * 1024u)
#endif
// ── The sample clock, and the window end every scope consumer wants ─────────
//
// Here rather than with the segment because it is pure arithmetic over the
// offsets above and a scope reader: no mapping, no POSIX, nothing a browser
// build cannot have. shm_segment.hpp re-exports it for callers that knew it
// there.
struct sample_clock_view {
    uint64_t engine_frames = 0;
    double   dac_ntp = 0.0;
    uint32_t sample_rate = 0;
    uint32_t output_latency_frames = 0;
    bool     valid = false;

    // Newest engine frame the listener has heard at wall time now_ntp.
    double visible_frames(double now_ntp) const {
        return static_cast<double>(engine_frames)
             + (now_ntp - dac_ntp) * static_cast<double>(sample_rate);
    }

    // Slot-local cursor of the newest AUDIBLE sample of a scope stream —
    // the canonical window end for every scope consumer. Clamped to the
    // stream's write position; a mapping that lands before the stream's
    // anchor (engine counter reset, wall-clock step) yields 0 so consumers
    // show silence rather than not-yet-heard audio. Falls back to the raw
    // write position when this view is invalid (no publisher yet:
    // headless engines, older builds).
    uint64_t audible_end(const shm_scope_stream_reader& r, double now_ntp) const {
        const uint64_t writer = r.write_position();
        if (!valid)
            return writer;
        const double local = visible_frames(now_ntp)
            - static_cast<double>(r.base_engine_frames());
        if (local <= 0.0)
            return 0;
        const uint64_t end = static_cast<uint64_t>(local);
        return end < writer ? end : writer;
    }
    // Readers share the host — and so the system clock — with the engine,
    // which anchors against the same wallClockNTP(): no cross-process
    // handshake needed.
    uint64_t audible_end(const shm_scope_stream_reader& r) const {
        return audible_end(r, wallClockNTP());
    }
};

// Field accesses are relaxed atomics (the DAC NTP travels as a u64 bit
// pattern) so no read is torn; the seqlock guards cross-field consistency.
inline sample_clock_view read_sample_clock(const uint8_t* tl) {
    sample_clock_view v;
    if (!tl) return v;
    auto* seq = reinterpret_cast<const std::atomic<uint32_t>*>(tl + SAMPLE_CLOCK_SEQ);
    auto* sr  = reinterpret_cast<const std::atomic<uint32_t>*>(tl + SAMPLE_CLOCK_SAMPLE_RATE);
    auto* fr  = reinterpret_cast<const std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_ENGINE_FRAMES);
    auto* nb  = reinterpret_cast<const std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_DAC_NTP);
    auto* lat = reinterpret_cast<const std::atomic<uint32_t>*>(tl + SAMPLE_CLOCK_OUT_LATENCY);
    for (int tries = 0; tries < 8; ++tries) {
        const uint32_t s0 = seq->load(std::memory_order_acquire);
        if (s0 == 0)
            return v;  // never published (seq is monotonic from 0)
        if (s0 & 1u)
            continue;  // writer mid-update
        const uint32_t r  = sr->load(std::memory_order_relaxed);
        const uint64_t f  = fr->load(std::memory_order_relaxed);
        const uint64_t nu = nb->load(std::memory_order_relaxed);
        const uint32_t l  = lat->load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq->load(std::memory_order_relaxed) == s0) {
            v.engine_frames = f;
            v.dac_ntp = clockwork::bitsToDouble(nu);
            v.sample_rate = r;
            v.output_latency_frames = l;
            v.valid = r > 0;
            return v;
        }
    }
    return v;
}

constexpr uint32_t GUEST_PERSIST_SIZE  = GUEST_PERSIST_BYTES;

// ── The channel map ─────────────────────────────────────────────────────────
//
// EVERY CHANNEL HAS AN IDENTITY, ALWAYS. Without this a DSP is handed an
// anonymous range: a stereo file's left and right are indistinguishable from
// each other and from the microphone, and a client picking somewhere to put a
// Link peer is guessing at an index and finding out by refusal.
//
// TWO HALVES, because the two readers want different things and paying for
// both everywhere is what made the earlier answers expensive.
//
//   HOT — one atomic word per channel. A DSP about to process channel c reads
//   in_channel[c] and knows what it is holding. The read IS the lookup: no
//   generation to remember, no changed-flag to consume, no cached copy to
//   invalidate, and it is current by construction. One relaxed load at the
//   point of use, and nothing at all when the DSP does not ask.
//
//   COLD — the stream table: name, direction, range. Variable-shaped, so it
//   cannot be one atomic word, and it is mostly what a CLIENT wants — to show
//   what is connected and to choose where the next stream goes. Stamped with a
//   generation that is ODD while it is being written.
//
// The generation exists for a reader that must not observe a half-written
// table. A client may ignore it: it is allowed to be a poll interval out of
// date, which costs it nothing and saves it a retry loop.
//
// WRITTEN ON THE CONTROL THREAD, when a binding changes — an attach, a detach,
// a device switch. Never per block. The audio thread only ever reads.
enum ClockworkChannelKind : uint32_t {
    CLOCKWORK_CH_NONE   = 0,   // nothing is bound here
    CLOCKWORK_CH_DEVICE = 1,   // the audio device's own channel
    CLOCKWORK_CH_PORT   = 2,   // a port (clockwork_ports.h) is bound over it
};

constexpr uint32_t CHANNEL_MAP_MAX_STREAMS = 64;
constexpr uint32_t CHANNEL_MAP_NAME_BYTES  = 64;

// Kind in the low byte; the stream's slot PLUS ONE in the next 16 bits, so
// zero means "no stream" and a device channel needs no slot at all.
constexpr uint32_t channelWord(uint32_t kind, uint32_t slotPlusOne) {
    return (kind & 0xFFu) | ((slotPlusOne & 0xFFFFu) << 8);
}
constexpr uint32_t channelKind(uint32_t w)   { return w & 0xFFu; }
constexpr uint32_t channelSlot(uint32_t w)   { return (w >> 8) & 0xFFFFu; }  // 0 = none

struct ClockworkStreamEntry {
    uint32_t port;        // ClockworkPort; 0 when the slot is free
    uint32_t direction;   // kClockworkPortSource (1) / kClockworkPortSink (2)
    uint32_t first;       // first channel of the range
    uint32_t count;       // how many
    char     name[CHANNEL_MAP_NAME_BYTES];   // NUL-terminated, truncated to fit
};

struct alignas(16) ClockworkChannelMapState {
    std::atomic<uint32_t> generation;    // odd while the cold half is written
    std::atomic<uint32_t> device_in;     // device input channels: [0, device_in)
    std::atomic<uint32_t> device_out;
    std::atomic<uint32_t> max_channels;  // how much of in/out is meaningful

    // Hot: indexed by channel, read at the point of use.
    std::atomic<uint32_t> in [CLOCKWORK_MAX_CHANNELS];
    std::atomic<uint32_t> out[CLOCKWORK_MAX_CHANNELS];

    // Cold: guarded by `generation`.
    ClockworkStreamEntry  streams[CHANNEL_MAP_MAX_STREAMS];
};

constexpr uint32_t CHANNEL_MAP_SIZE  = sizeof(ClockworkChannelMapState);

// ── Client slots ────────────────────────────────────────────────────────────
//
// SOMEWHERE FOR A CLIENT TO STAND. A client that shares the engine's address
// space still needs a few bytes of its own: somewhere to put the handle, and a
// stack to run on. In a process that is trivial — it calls malloc. In a
// WebAssembly build it is not, because "the engine's address space" is one
// linear memory shared by the audio worklet and every worker, and a second
// module instance over it starts life pointing at the SAME stack the worklet
// is using. Its first call would run over the audio thread's frame.
//
// So the memory is laid out here instead, where it needs no allocator and no
// handshake: a context claims a slot with one atomic compare-exchange, aims
// its stack pointer at that slot, and opens its handle into it with
// clockwork_client_open_memory_in. Nothing has to have booted first, and no
// two contexts can be handed the same bytes.
//
// Native clients use none of this. They have a heap.
// Three are spoken for in a browser — the main thread, the reply pump and the
// traffic logger — and the rest are for workers an embedder gives a channel to.
// Running out is reported at open rather than shared, so the ceiling is a
// number to raise, never a corruption to debug.
constexpr uint32_t CLIENT_SLOT_COUNT   = 8;

// Handle, tap, out-parameters and a poll array, in that order.
constexpr uint32_t CLIENT_SLOT_STRUCTS_SIZE = 8192;
// The client path is shallow — frame a message, walk a ring — so this is
// generous rather than tight.
constexpr uint32_t CLIENT_SLOT_STACK_SIZE   = 64 * 1024;

// There is no staging buffer here, and that is deliberate. A client builds its
// message inside the ring through clockwork_client_send_begin, so the bytes
// are written once, where they are going to live. A slot holding a buffer to
// assemble messages in would be paying for a copy nobody needs.
constexpr uint32_t CLIENT_SLOT_STRUCTS_OFFSET = 0;
constexpr uint32_t CLIENT_SLOT_STACK_OFFSET =
    CLIENT_SLOT_STRUCTS_OFFSET + CLIENT_SLOT_STRUCTS_SIZE;
constexpr uint32_t CLIENT_SLOT_SIZE =
    CLIENT_SLOT_STACK_OFFSET + CLIENT_SLOT_STACK_SIZE;

// One uint32 of claim bits, and room to grow without moving the slots.
constexpr uint32_t CLIENT_SLOTS_HEADER_SIZE = 16;
constexpr uint32_t CLIENT_SLOTS_SIZE =
    CLIENT_SLOTS_HEADER_SIZE + CLIENT_SLOT_COUNT * CLIENT_SLOT_SIZE;

static_assert(CLIENT_SLOT_COUNT <= 32,
              "the claim bitmap is one uint32");

// ============================================================================
// THE ARENA MAP
// ============================================================================
// Every region's place, in one list, in the order it lies in memory. Two
// halves (clockwork_arena.h says why): the CLOCKWORK BLOCK first — the
// header, the small engine state, the rings, the taps, the client slots — and
// the GUEST REGION after it. Nothing here is read by anyone but the engine
// and its tests: readers take the table the header carries (writeArenaHeader
// below), which is filled from these.
//
// The order is chosen, not accumulated. Small state that is read every block
// sits together at the front; the rings follow; the large arrays last, each
// 16-aligned because their slot structs are alignas(16).
constexpr uint32_t alignArena16(uint32_t v) { return (v + 15u) & ~15u; }
constexpr uint32_t alignArena8 (uint32_t v) { return (v + 7u)  & ~7u;  }

// ── the clockwork block ──
constexpr uint32_t ARENA_HEADER_START   = 0;
constexpr uint32_t ARENA_HEADER_SIZE    = CLOCKWORK_ARENA_HEADER_BYTES;
constexpr uint32_t CONTROL_START        = ARENA_HEADER_START + ARENA_HEADER_SIZE;
constexpr uint32_t METRICS_START        = CONTROL_START + CONTROL_SIZE;
constexpr uint32_t NATIVE_STATS_START   = METRICS_START + METRICS_SIZE;
// The host's clock anchors, as one region: the NTP start time (f64) then the
// two offsets. The three names below are what the code has always used.
constexpr uint32_t CLOCK_ANCHORS_START  = alignArena8(NATIVE_STATS_START + NATIVE_STATS_SIZE);
constexpr uint32_t NTP_START_TIME_START = CLOCK_ANCHORS_START;
constexpr uint32_t DRIFT_OFFSET_START   = NTP_START_TIME_START + NTP_START_TIME_SIZE;
constexpr uint32_t GLOBAL_OFFSET_START  = DRIFT_OFFSET_START + DRIFT_OFFSET_SIZE;
constexpr uint32_t CLOCK_ANCHORS_SIZE   = NTP_START_TIME_SIZE + DRIFT_OFFSET_SIZE + GLOBAL_OFFSET_SIZE;
constexpr uint32_t CLOCK_STATE_START    = alignArena8(CLOCK_ANCHORS_START + CLOCK_ANCHORS_SIZE);
constexpr uint32_t SAMPLE_CLOCK_START   = alignArena16(CLOCK_STATE_START + CLOCK_STATE_SIZE);
constexpr uint32_t NODE_ID_COUNTER_START = SAMPLE_CLOCK_START + SAMPLE_CLOCK_SIZE;
constexpr uint32_t CHANNEL_MAP_START    = alignArena16(NODE_ID_COUNTER_START + NODE_ID_COUNTER_SIZE);
constexpr uint32_t IN_BUFFER_START      = alignArena16(CHANNEL_MAP_START + CHANNEL_MAP_SIZE);
constexpr uint32_t OUT_BUFFER_START     = IN_BUFFER_START + IN_BUFFER_SIZE;
constexpr uint32_t NRT_OUT_BUFFER_START = OUT_BUFFER_START + OUT_BUFFER_SIZE;
constexpr uint32_t SHM_AUDIO_START      = alignArena16(NRT_OUT_BUFFER_START + NRT_OUT_BUFFER_SIZE);
constexpr uint32_t SHM_TRACK_TAPS_START = alignArena16(SHM_AUDIO_START + SHM_AUDIO_TOTAL_SIZE);
constexpr uint32_t CLIENT_SLOTS_START   = alignArena16(SHM_TRACK_TAPS_START + SHM_TRACK_TAPS_SIZE);
constexpr uint32_t CLOCKWORK_BLOCK_SIZE = alignArena16(CLIENT_SLOTS_START + CLIENT_SLOTS_SIZE);

// ── the guest region ──
constexpr uint32_t GUEST_REGION_START   = CLOCKWORK_BLOCK_SIZE;
constexpr uint32_t GUEST_CONFIG_START   = GUEST_REGION_START;
constexpr uint32_t SHM_WINDOW_START     = GUEST_CONFIG_START + GUEST_CONFIG_SIZE;
constexpr uint32_t SHM_SCOPE_START      = alignArena16(SHM_WINDOW_START + SHM_WINDOW_SIZE);
constexpr uint32_t GUEST_PERSIST_START  = alignArena16(SHM_SCOPE_START + SHM_SCOPE_TOTAL_SIZE);
constexpr uint32_t TOTAL_BUFFER_SIZE    = GUEST_PERSIST_START + GUEST_PERSIST_SIZE;
constexpr uint32_t GUEST_REGION_SIZE    = TOTAL_BUFFER_SIZE - GUEST_REGION_START;

static_assert(sizeof(ClockworkArenaHeader) <= ARENA_HEADER_SIZE, "the arena header outgrew its reservation");
static_assert(CONTROL_START % 16 == 0,      "the control block must be 16-byte aligned");
static_assert(CLOCK_STATE_START % 8 == 0,   "ClockworkClockState holds 64-bit atomics");
static_assert(SAMPLE_CLOCK_START % 16 == 0, "the sample clock is read as 64-bit words");
static_assert(CHANNEL_MAP_START % 16 == 0,  "ClockworkChannelMapState is alignas(16)");
static_assert(IN_BUFFER_START % 16 == 0,    "rings start 16-aligned");
static_assert(SHM_AUDIO_START % 16 == 0,    "shm_audio_buffer is alignas(16)");
static_assert(SHM_TRACK_TAPS_START % 16 == 0, "shm_scope_stream is alignas(16)");
static_assert(CLIENT_SLOTS_START % 16 == 0, "client slots are 16-byte aligned");
static_assert(GUEST_REGION_START % 16 == 0, "the guest region starts 16-aligned");
static_assert((SHM_SCOPE_START + SHM_SCOPE_HEADER_SIZE) % 16 == 0,
              "scope slots must be 16-byte aligned for shm_scope_stream");
static_assert(GUEST_PERSIST_START % 16 == 0, "the guest's persistent bytes start 16-aligned");

// Message frame (magic/length/sequence/sourceId) is defined in ring/ring.h.

// Egress framing: every frame on both egress rings is
// Message{sourceId = origin token} + [route:u32][osc].
enum EgressRoute : uint32_t {
    EGRESS_REPLY            = 0,  // reply to the origin token
    EGRESS_SEND_TO_CALLER   = 1,  // reply to the origin token, network peers only
    EGRESS_BROADCAST_NOTIFY = 2,  // fan out to all notify subscribers
    EGRESS_BROADCAST_LINK   = 3,  // fan out to all Link subscribers
    EGRESS_BROADCAST_MIDI   = 4,  // fan out to all MIDI-notify subscribers
    EGRESS_BROADCAST_GAMEPAD = 5, // fan out to all gamepad-notify subscribers
    EGRESS_BROADCAST_OSC    = 6,  // fan out to all external-OSC-cue subscribers
};
constexpr uint32_t EGRESS_ROUTE_SIZE = sizeof(uint32_t);  // leading route word

// Control pointers structure (4-byte aligned for atomics, padded to 48 bytes for 8-byte alignment)
struct alignas(4) ControlPointers {
    std::atomic<int32_t> in_head;
    std::atomic<int32_t> in_tail;
    std::atomic<int32_t> out_head;
    std::atomic<int32_t> out_tail;
    std::atomic<int32_t> nrt_out_head;
    std::atomic<int32_t> nrt_out_tail;
    std::atomic<int32_t> in_sequence;     // Sequence counter for IN buffer (shared between main thread & worker)
    std::atomic<int32_t> out_sequence;    // Sequence counter for OUT buffer
    std::atomic<int32_t> nrt_out_sequence;  // Sequence counter for the NRT-out buffer
    std::atomic<uint32_t> status_flags;
    std::atomic<int32_t> in_write_lock;   // Spinlock for IN buffer writes (0=unlocked, 1=locked)
    int32_t _padding;                     // Padding to maintain 8-byte alignment for subsequent Float64
};

// Performance metrics structure
// Layout designed for contiguous memcpy operations:
// - [0-8]   engine (shared C++; wasm_errors is JS-only)
// - [9-10]  OSC Out (native ingest + JS)
// - [11-14] OSC In (native egress drain + JS osc_in_worker)
// - [15-16] Debug (shared C++)
// - [17-19] Ring buffer usage (shared C++)
// - [20-22] Ring buffer peak usage (shared C++)
// - [23-25] engine late timing diagnostics (shared C++)
// - [26]    Ring buffer direct write failures (OscChannel SAB mode, JS-only)
// - [27-38] Link (native-only)
// - [39-45] System info: version + audio config (shared C++, write-once)
// - [46-49] ClockworkClock readouts: tempo/beat/phase/playing (shared C++, per block)
struct alignas(4) PerformanceMetrics {
    // The same struct is written from native (binary + NIF) and web (WASM/JS).
    // Each group below identifies its writers by runtime. "shared C++" means
    // the writer is in shared source (audio_processor.cpp / engine core) and
    // runs on all three runtimes. Fields tagged JS-only stay 0 on native;
    // fields tagged native-only stay 0 on web.

    // engine core metrics [0-8]
    // Writers: shared C++ in audio_processor.cpp (runs on all runtimes).
    // Exception: wasm_errors is JS-only (worklet-side WASM exec errors).
    // Exception: messages_sequence_gaps also written by the native egress drains
    // (lanes.cpp / ring_drain.h).
    std::atomic<uint32_t> process_count;           // 0: Audio process() callbacks
    std::atomic<uint32_t> messages_processed;      // 1: OSC messages processed
    std::atomic<uint32_t> messages_dropped;        // 2: Messages dropped
    std::atomic<uint32_t> scheduler_queue_depth;   // 3: Current scheduler depth
    std::atomic<uint32_t> scheduler_queue_max;     // 4: Peak scheduler depth
    std::atomic<uint32_t> scheduler_queue_dropped; // 5: Scheduler overflow drops
    std::atomic<uint32_t> messages_sequence_gaps;  // 6: Sequence gaps detected
    std::atomic<uint32_t> wasm_errors;             // 7: WASM execution errors (JS-only)
    std::atomic<uint32_t> scheduler_lates;         // 8: Bundles executed after scheduled time

    // OSC Out metrics [9-10] — client perspective: messages the host SENDS to the
    // engine (written into the IN ring). Counted at the producer end, so this is
    // the host→engine direction and pairs with `messages_processed` (#1), which
    // counts the same stream as the engine drains it off the IN ring.
    // Writers: native — ClockworkEngine::ingest(); web — js/lib/osc_channel.js (SAB
    // mode) and js/workers/clockwork_audio_worklet.js.
    std::atomic<uint32_t> osc_out_messages_sent;   // 9
    std::atomic<uint32_t> osc_out_bytes_sent;      // 10

    // OSC In metrics [11-14] — client perspective: messages the host RECEIVES from
    // the engine (drained from the OUT ring as replies/notifications). This is the
    // engine→host direction — the OPPOSITE of the "in" ring (host→engine); the
    // names follow the client's point of view, the ring-usage block below the
    // engine's. osc_in_dropped_messages is JS-only (no native writer).
    // Writers: native — clockwork_egress_rt_drain() (lanes.cpp); web — js/workers/osc_in_worker.js.
    std::atomic<uint32_t> osc_in_messages_received; // 11
    std::atomic<uint32_t> osc_in_bytes_received;    // 12
    std::atomic<uint32_t> osc_in_dropped_messages;  // 13 (JS-only)
    std::atomic<uint32_t> osc_in_corrupted;         // 14: Ring buffer message corruption

    // Debug metrics [15-16]
    // Writers: shared C++ — emit_debug_osc() in audio_processor.cpp counts every
    // debug line it emits, on all runtimes.
    std::atomic<uint32_t> debug_messages_received;  // 15
    std::atomic<uint32_t> debug_bytes_received;     // 16

    // Ring buffer usage [17-19]
    // Writers: shared C++ during process() (audio_processor.cpp / engine core).
    std::atomic<uint32_t> in_buffer_used_bytes;     // 17: Bytes used in IN buffer
    std::atomic<uint32_t> out_buffer_used_bytes;    // 18: Bytes used in OUT buffer
    std::atomic<uint32_t> nrt_out_buffer_used_bytes;  // 19: Bytes used in NRT-out buffer

    // Ring buffer peak usage [20-22]
    // Writers: shared C++ during process() (audio_processor.cpp / engine core).
    std::atomic<uint32_t> in_buffer_peak_bytes;     // 20: Peak bytes used in IN buffer
    std::atomic<uint32_t> out_buffer_peak_bytes;    // 21: Peak bytes used in OUT buffer
    std::atomic<uint32_t> nrt_out_buffer_peak_bytes;  // 22: Peak bytes used in NRT-out buffer

    // engine late timing diagnostics [23-25]
    // Writers: shared C++ during process() (audio_processor.cpp).
    std::atomic<int32_t> scheduler_max_late_ms;     // 23: Maximum lateness observed (ms)
    std::atomic<int32_t> scheduler_last_late_ms;    // 24: Most recent late magnitude (ms)
    std::atomic<uint32_t> scheduler_last_late_tick; // 25: Process count when last late occurred

    // Ring buffer direct write failures [26]
    // Writer: JS-only — OscChannel in SAB mode increments when an optimistic
    // direct ring write fails and the bundle is dropped.
    // C++ side reserves the slot but doesn't read or write it.
    std::atomic<uint32_t> ring_buffer_direct_write_fails; // 26

    // ─── Link (native-only; 0 on WASM) ──────────────────────────────────
    // Clock readouts mirrored from ClockworkClock each block; floats stored as
    // fixed-point (decoded by the panel's display formats).
    std::atomic<uint32_t> link_peers;              // 27: connected Link peers
    std::atomic<uint32_t> link_tempo_mbpm;         // 28: tempo, milli-BPM (bpm * 1000)
    std::atomic<uint32_t> link_beat_centi;         // 29: beat position * 100
    std::atomic<uint32_t> link_phase_centi;        // 30: phase within quantum * 100
    std::atomic<uint32_t> link_playing;            // 31: transport 0/1

    // ─── Link Audio stream health (native-only; 0 on WASM) ──────────────
    std::atomic<uint32_t> link_audio_in_channels;  // 32: active received channels
    std::atomic<uint32_t> link_audio_stream_rate;  // 33: received stream sample rate (Hz)
    std::atomic<uint32_t> link_audio_underruns;    // 34: receiver queue underrun events
    std::atomic<uint32_t> link_audio_buffered_ms;  // 35: receiver queue depth (ms)
    std::atomic<int32_t>  link_audio_drift_ppm;    // 36: read-rate deviation from 1.0 (ppm, signed)
    std::atomic<uint32_t> link_audio_publish;      // 37: publishing enabled 0/1
    std::atomic<uint32_t> link_audio_sinks;        // 38: active output sinks

    // ─── System info (cross-platform; written by shared C++ on every runtime) ─
    // Engine identity + audio connection details. Written once at init by
    // init_memory() (audio_processor.cpp). Constant for the session.
    std::atomic<uint32_t> clockwork_version_major; // 39: CLOCKWORK_VERSION_MAJOR
    std::atomic<uint32_t> clockwork_version_minor; // 40: CLOCKWORK_VERSION_MINOR
    std::atomic<uint32_t> clockwork_version_patch; // 41: CLOCKWORK_VERSION_PATCH
    std::atomic<uint32_t> audio_sample_rate;        // 42: output sample rate (Hz)
    std::atomic<uint32_t> audio_block_size;         // 43: block size (frames; 128 on web)
    std::atomic<uint32_t> audio_output_channels;    // 44: output bus channels
    std::atomic<uint32_t> audio_input_channels;     // 45: input bus channels

    // ─── ClockworkClock readouts (cross-platform; written per block) ─────────────
    // Mirrored from ClockworkClock each block by publishClockMetrics() — sourced
    // from the ClockworkClockState SAB mirror, so live on web and native alike
    // (independent of Link). Floats stored as fixed-point (see web schema /
    // panel display formats).
    std::atomic<uint32_t> clock_tempo_mbpm;         // 46: tempo, milli-BPM (bpm * 1000)
    std::atomic<uint32_t> clock_beat_centi;         // 47: beat position * 100
    std::atomic<uint32_t> clock_phase_centi;        // 48: phase within quantum * 100
    std::atomic<uint32_t> clock_playing;            // 49: transport 0/1

    // Reserved padding so METRICS_SIZE stays a multiple of 8 bytes — the
    // regions that follow in the arena (NTP time, ClockworkClockState) are
    // 8-byte-aligned and read via Float64/BigInt64 views, which require it.
    // Two words are needed to keep the 51-meaningful-field struct (0-49 plus
    // _metrics_reserved at 50) padded to a multiple of 8.
    std::atomic<uint32_t> _metrics_reserved;        // 50: reserved (alignment)
    std::atomic<uint32_t> _metrics_reserved2;       // 51: reserved (alignment pad)
};

// ClockworkClock session state. Has its own SAB region because it's engine
// state (the JS↔worklet transport on WASM depends on it), not observability
// — separating it from PerformanceMetrics keeps that struct honest.
//
// Bound on EVERY build: WASM at clockwork_clock_wasm_init, native at engine init
// (ClockworkEngine.cpp -> bindStateToShm).
//
// Each field is a single 64-bit atomic (doubles stored as IEEE 754 bit-
// pattern). There is no seqlock. Coherence between the fields that MUST agree
// comes from a directional protocol instead: the writer stores the value
// first (relaxed) and the KEY field last (release), so a reader that
// acquire-loads the key is guaranteed to see the value anchored for it.
//
//   pair                              key field
//   beat_origin_ntp + bpm             bpm
//   is_playing_at_ntp + is_playing    is_playing
//
// The meter is the one two-part value that is NOT a pair: numerator and
// denominator travel packed in one word (packMeter), so a reader can never
// see a 7 over the old 4.
//
// That contract is easy to get wrong, so nobody implements it in place:
// writers go through the mutators on the struct (setTempo / retempo /
// setOrigin / setTransport / setFlag / copyFrom — test_clock_state.cpp) and
// readers call readClockworkClock() below, which is the only reader anyone needs.
// The JS twins are in js/lib/clockwork_clock_protocol.js.
// The zero initialisers are required, not decoration. A std::atomic with no
// initialiser is INDETERMINATE under default-initialisation, and this struct is
// not only mapped over shared memory — ClockworkClock::Impl declares one as an
// ordinary member (`ownedState`), the private state a clock answers from before
// the engine binds it into the arena. Without these, a freshly constructed
// ClockworkClock reported whatever the stack held: isLinkEnabled() came back true
// on a build with no Link compiled in at all.
//
// Mapping a region and casting to this type is unaffected — nothing is
// constructed there, so nothing is written. initDefaults() below is still the
// way to get a *useful* state (it is the only thing that knows the default
// tempo); these merely guarantee that "not yet initialised" reads as zero
// rather than as garbage.
struct alignas(8) ClockworkClockState {
    std::atomic<uint64_t> bpm{0};                  // 0-7:  BPM as IEEE 754 bit-pattern
    std::atomic<uint64_t> beat_origin_ntp{0};      // 8-15: NTP seconds as bit-pattern
    std::atomic<uint64_t> is_playing_at_ntp{0};    // 16-23: NTP seconds as bit-pattern
    std::atomic<uint32_t> is_playing{0};           // 24-27: 0 = stopped, 1 = playing
    std::atomic<uint32_t> flags{0};                // 28-31: bit-packed session flags
    std::atomic<uint32_t> meter{0};                // 32-35: (num << 16) | den, see packMeter
    std::atomic<uint32_t> _clock_reserved{0};      // 36-39: alignment pad (the region is 8-byte)

    // The meter as one word, so the two halves are read together. Both
    // halves fit 16 bits with room to spare (a meter is small integers).
    static uint32_t packMeter(int32_t num, int32_t den) {
        return (static_cast<uint32_t>(num) << 16) | (static_cast<uint32_t>(den) & 0xFFFFu);
    }
    static int32_t meterNum(uint32_t packed) { return static_cast<int32_t>(packed >> 16); }
    static int32_t meterDen(uint32_t packed) { return static_cast<int32_t>(packed & 0xFFFFu); }

    static void initDefaults(ClockworkClockState& s) {
        s.bpm.store(clockwork::doubleToBits(clockwork::kDefaultBpm), std::memory_order_relaxed);
        s.beat_origin_ntp.store(0u,                  std::memory_order_relaxed);
        s.is_playing_at_ntp.store(0u,                std::memory_order_relaxed);
        s.is_playing.store(0u,                       std::memory_order_relaxed);
        s.flags.store(0u,                            std::memory_order_relaxed);
        s.meter.store(packMeter(4, 4),               std::memory_order_relaxed);
        s._clock_reserved.store(0u,                  std::memory_order_relaxed);
    }

    // ── The write protocol, once ──────────────────────────────────────────
    // Tempo below 1 is clamped: requestBeatAtTime / timeAtBeat divide by it,
    // and 0 or NaN would write ±inf/NaN into the origin and poison every read.
    static double clampBpm(double bpm) { return bpm >= 1.0 ? bpm : 1.0; }

    // Publish a grid: origin first, then the tempo with release, so a reader
    // that acquire-loads the tempo sees the origin anchored for it.
    void setTempo(double bpmIn, double originNtp) {
        beat_origin_ntp.store(clockwork::doubleToBits(originNtp), std::memory_order_relaxed);
        bpm.store(clockwork::doubleToBits(clampBpm(bpmIn)), std::memory_order_release);
    }

    // Change the tempo without moving the beat playing at `nowNtp`
    // (clockwork::retempoOrigin). Returns that beat.
    double retempo(double bpmIn, double nowNtp) {
        const double newBpm = clampBpm(bpmIn);
        const double oldBpm = clockwork::bitsToDouble(bpm.load(std::memory_order_relaxed));
        const double origin = clockwork::bitsToDouble(beat_origin_ntp.load(std::memory_order_relaxed));
        const double held   = (origin != 0.0 && oldBpm >= 1.0)
                            ? clockwork::beatAt(nowNtp, origin, oldBpm) : 0.0;
        setTempo(newBpm, clockwork::retempoOrigin(origin, oldBpm, newBpm, nowNtp));
        return held;
    }

    // Move the grid under the current tempo (requestBeatAtTime and friends).
    void setOrigin(double originNtp) {
        beat_origin_ntp.store(clockwork::doubleToBits(originNtp), std::memory_order_relaxed);
    }

    // Timestamp first, then the flag with release — same shape as setTempo.
    void setTransport(bool playing, double atNtp) {
        is_playing_at_ntp.store(clockwork::doubleToBits(atNtp), std::memory_order_relaxed);
        is_playing.store(playing ? 1u : 0u, std::memory_order_release);
    }

    // One bit, siblings untouched.
    void setFlag(uint32_t mask, bool on) {
        if (on) flags.fetch_or(mask,   std::memory_order_relaxed);
        else    flags.fetch_and(~mask, std::memory_order_relaxed);
    }

    // The meter, as one word. Validation is the caller's (ClockworkClock::setMeter
    // asks clockwork_timeline_meter_valid); the grid is untouched, because bars are
    // counted from beat 0 whatever the meter.
    void setMeter(int32_t num, int32_t den) {
        meter.store(packMeter(num, den), std::memory_order_relaxed);
    }

    // A coherent copy: read `src` under its protocol, write here under ours.
    inline void copyFrom(const ClockworkClockState& src);
};

// A coherent snapshot of the session clock.
//
// This exists because the DSP consumes the clock too, and a DSP is written by
// someone who should not have to know the ordering rules above. Read the key
// field with acquire, then the value it anchors: that is the whole protocol,
// implemented once, here.
struct ClockworkClockSnapshot {
    double   bpm;                // beats per minute
    double   beat_origin_ntp;    // NTP seconds at which beat 0 occurred
    double   is_playing_at_ntp;  // NTP seconds the transport last changed
    bool     is_playing;
    uint32_t flags;              // SC_FLAG_*
    int32_t  meter_num;          // how quarter-note beats group into bars
    int32_t  meter_den;

    // The beat at an NTP instant, under this snapshot.
    double beatAt(double ntpSeconds) const {
        return clockwork::beatAt(ntpSeconds, beat_origin_ntp, bpm);
    }
};

inline ClockworkClockSnapshot readClockworkClock(const ClockworkClockState* s) {
    ClockworkClockSnapshot out{};
    if (!s) {
        out.bpm = clockwork::kDefaultBpm;
        out.meter_num = 4;
        out.meter_den = 4;
        return out;
    }
    // bpm is the key for beat_origin_ntp; is_playing is the key for its
    // timestamp. Acquire first, then the anchored value.
    out.bpm = clockwork::bitsToDouble(s->bpm.load(std::memory_order_acquire));
    out.beat_origin_ntp =
        clockwork::bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed));
    out.is_playing = s->is_playing.load(std::memory_order_acquire) != 0u;
    out.is_playing_at_ntp =
        clockwork::bitsToDouble(s->is_playing_at_ntp.load(std::memory_order_relaxed));
    out.flags = s->flags.load(std::memory_order_relaxed);
    const uint32_t meter = s->meter.load(std::memory_order_relaxed);
    out.meter_num = ClockworkClockState::meterNum(meter);
    out.meter_den = ClockworkClockState::meterDen(meter);
    return out;
}

inline void ClockworkClockState::copyFrom(const ClockworkClockState& src) {
    // A faithful copy, not a sanitising one: the bits travel as they are (a
    // region that was never initDefaults'd copies as the zeros it holds), in
    // the same value-then-key order the mutators use.
    const ClockworkClockSnapshot in = readClockworkClock(&src);
    beat_origin_ntp.store(clockwork::doubleToBits(in.beat_origin_ntp), std::memory_order_relaxed);
    bpm.store(clockwork::doubleToBits(in.bpm), std::memory_order_release);
    setTransport(in.is_playing, in.is_playing_at_ntp);
    flags.store(in.flags, std::memory_order_relaxed);
    setMeter(in.meter_num, in.meter_den);
}

// Bit positions inside ClockworkClockState::flags. Single atomic uint32 so
// readers can snapshot all flags in one load; writers use fetch_or /
// fetch_and to mutate individual bits without stomping siblings.
constexpr uint32_t SC_FLAG_LINK_ENABLED         = 1u << 0;
constexpr uint32_t SC_FLAG_START_STOP_SYNC      = 1u << 1;
constexpr uint32_t SC_FLAG_LINK_AUDIO_PUBLISH   = 1u << 2;
static_assert(sizeof(ClockworkClockState) == CLOCK_STATE_SIZE,
              "ClockworkClockState size must match CLOCK_STATE_SIZE");

enum StatusFlags : uint32_t {
    STATUS_OK = 0,
    STATUS_BUFFER_FULL = 1 << 0,
    STATUS_OVERRUN = 1 << 1,
    STATUS_WASM_ERROR = 1 << 2,
    STATUS_FRAGMENTED_MSG = 1 << 3
};

// Constants
// MAX_MESSAGE_SIZE bounds what a frame header on the INGRESS ring may CLAIM.
// It is not a bound for the egress rings and must not be used as one: it is
// derived from IN_BUFFER_SIZE, which is six times OUT_BUFFER_SIZE and twelve
// times NRT_OUT_BUFFER_SIZE by default, so a reader validating an egress frame
// against this would accept a length several rings long. The C++ readers do
// not — ring_drain.h bounds every frame by the size of the ring it was handed,
// which is the only correct source — and this is exported in BufferLayout for
// JavaScript, where nothing currently reads it.
//
// It is not what is guaranteed WRITABLE either: frames never wrap the ring
// boundary, so the largest frame guaranteed to fit depends on where the idle
// cursors sit — worst case roughly half the ring. Senders learn the real
// answer per-write from the writer's fit check (canWriteMessage / the writers'
// false return), and a guest learns it from DspHost::emit_osc's return.
constexpr uint32_t MAX_MESSAGE_SIZE = IN_BUFFER_SIZE - sizeof(Message);
// MESSAGE_MAGIC / PADDING_MAGIC are defined in ring/ring.h.
constexpr uint8_t RING_PADDING_MARKER = 0xFF;  // Byte marking ring-buffer padding (skip to position 0 on wrap)

// Scheduler configuration is sized per device via memory_profile.h
// (defaults: SCHEDULER_DATA_POOL_SIZE 512KB, SCHEDULER_SLOT_COUNT 512) and is
// shared with the scheduler, which is built from the same profile.

// ============================================================================
// THE READER'S TABLE
// ============================================================================
// ── What a reader needs to know, as data ────────────────────────────────────
//
// The arena header (clockwork_arena.h) carries these values, and a reader
// takes them from there rather than from the constants above, so a reader
// built from another memory profile — or from no engine tree at all, through
// clockwork_client.h — still finds every region where THIS engine put it.
// The engine fills the header from its constants (writeArenaHeader); a reader
// fills this from the header (from_arena) and runs check() against it before
// trusting an offset.
//
// What check() pins is not equality with this build but the two things a
// reader cannot do without: every region lies inside the blob, and every
// fixed-shape struct the reader casts to (control, metrics, clock state,
// sample clock, channel map, the slot headers) is at least as large as this
// build's definition of it. Ring sizes, slot counts, ring capacities and the
// window are the engine's to choose, and the reader follows.
struct ShmReaderLayout {
    uint32_t blob_size = 0;
    uint32_t in_ring_offset = 0,      in_ring_size = 0;
    uint32_t out_ring_offset = 0,     out_ring_size = 0;
    uint32_t nrt_out_ring_offset = 0, nrt_out_ring_size = 0;
    uint32_t control_offset = 0,      control_bytes = 0;
    uint32_t metrics_offset = 0,      metrics_bytes = 0;
    uint32_t window_offset = 0,       window_bytes = 0;
    uint32_t clock_state_offset = 0,  clock_state_bytes = 0;
    uint32_t audio_offset = 0, audio_slot_count = 0, audio_slot_bytes = 0;
    uint32_t audio_header_bytes = 0, audio_frames = 0, audio_channels = 0, audio_sample_rate = 0;
    uint32_t scope_offset = 0, scope_max = 0, scope_header_bytes = 0, scope_slot_bytes = 0;
    uint32_t scope_slot_header = 0, scope_ring_frames = 0, scope_channels = 0;
    // The engine's track-tap slots: the same shape, numbered from track_first.
    // Zero slots on a build that carves none.
    uint32_t track_offset = 0, track_slots = 0, track_first = 0;
    uint32_t native_stats_offset = 0, native_stats_bytes = 0;
    uint32_t sample_clock_offset = 0, sample_clock_bytes = 0;
    uint32_t channel_map_offset = 0,  channel_map_bytes = 0;

    // The engine's own table: what writeArenaHeader publishes.
    static ShmReaderLayout from_constants() {
        ShmReaderLayout L;
        L.blob_size           = TOTAL_BUFFER_SIZE;
        L.in_ring_offset      = IN_BUFFER_START;      L.in_ring_size      = IN_BUFFER_SIZE;
        L.out_ring_offset     = OUT_BUFFER_START;     L.out_ring_size     = OUT_BUFFER_SIZE;
        L.nrt_out_ring_offset = NRT_OUT_BUFFER_START; L.nrt_out_ring_size = NRT_OUT_BUFFER_SIZE;
        L.control_offset      = CONTROL_START;        L.control_bytes     = CONTROL_SIZE;
        L.metrics_offset      = METRICS_START;        L.metrics_bytes     = METRICS_SIZE;
        L.window_offset       = SHM_WINDOW_START;     L.window_bytes      = SHM_WINDOW_SIZE;
        L.clock_state_offset  = CLOCK_STATE_START;    L.clock_state_bytes = CLOCK_STATE_SIZE;
        L.audio_offset        = SHM_AUDIO_START;
        L.audio_slot_count    = SHM_AUDIO_SLOTS;
        L.audio_slot_bytes    = SHM_AUDIO_SLOT_SIZE;
        L.audio_header_bytes  = SHM_AUDIO_HEADER_SIZE;
        L.audio_frames        = SHM_AUDIO_FRAMES;
        L.audio_channels      = SHM_AUDIO_CHANNELS;
        L.audio_sample_rate   = SHM_AUDIO_SAMPLE_RATE;
        L.scope_offset        = SHM_SCOPE_START;
        L.scope_max           = SHM_SCOPE_MAX_SCOPES;
        L.scope_header_bytes  = SHM_SCOPE_HEADER_SIZE;
        L.scope_slot_bytes    = SHM_SCOPE_SLOT_SIZE;
        L.scope_slot_header   = SHM_SCOPE_SLOT_HEADER_SIZE;
        L.scope_ring_frames   = SHM_SCOPE_RING_FRAMES;
        L.scope_channels      = SHM_SCOPE_CHANNELS;
        L.track_offset        = SHM_TRACK_TAPS_START;
        L.track_slots         = SHM_TRACK_TAPS_SLOTS;
        L.track_first         = SHM_SCOPE_TRACK_SLOT_BASE;
        L.native_stats_offset = NATIVE_STATS_START;   L.native_stats_bytes = NATIVE_STATS_SIZE;
        L.sample_clock_offset = SAMPLE_CLOCK_START;   L.sample_clock_bytes = SAMPLE_CLOCK_SIZE;
        L.channel_map_offset  = CHANNEL_MAP_START;    L.channel_map_bytes  = CHANNEL_MAP_SIZE;
        return L;
    }

    // A reader's table, from the header at the front of `base`. False, with
    // `why`, when the header is not one this reader can use — the wrong
    // magic or version, a table that does not fit in `bytes`, a region the
    // reader needs that the arena has not got. Acquires after the magic, so
    // a table seen published is a table fully written.
    static bool from_arena(const uint8_t* base, uint32_t bytes, ShmReaderLayout& L,
                           const char** why) {
        const auto* h = reinterpret_cast<const ClockworkArenaHeader*>(base);
        if (!clockwork_arena_check(h, bytes, why)) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto need = [&](uint32_t id, const char* w) -> const ClockworkArenaEntry* {
            const auto* e = clockwork_arena_find(h, id);
            if (!e && why) *why = w;
            return e;
        };
        const auto* in   = need(CLOCKWORK_ARENA_IN_RING,      "arena has no ingress ring");
        const auto* out  = need(CLOCKWORK_ARENA_OUT_RING,     "arena has no egress ring");
        const auto* nrt  = need(CLOCKWORK_ARENA_NRT_OUT_RING, "arena has no NRT egress ring");
        const auto* ctl  = need(CLOCKWORK_ARENA_CONTROL,      "arena has no control block");
        const auto* met  = need(CLOCKWORK_ARENA_METRICS,      "arena has no metrics");
        const auto* win  = need(CLOCKWORK_ARENA_GUEST_WINDOW, "arena has no guest window");
        const auto* clk  = need(CLOCKWORK_ARENA_CLOCK_STATE,  "arena has no clock state");
        const auto* taps = need(CLOCKWORK_ARENA_AUDIO_TAPS,   "arena has no audio taps");
        const auto* sc   = need(CLOCKWORK_ARENA_SCOPE,        "arena has no scope streams");
        const auto* ns   = need(CLOCKWORK_ARENA_NATIVE_STATS, "arena has no native stats");
        const auto* smp  = need(CLOCKWORK_ARENA_SAMPLE_CLOCK, "arena has no sample clock");
        const auto* cm   = need(CLOCKWORK_ARENA_CHANNEL_MAP,  "arena has no channel map");
        if (!in || !out || !nrt || !ctl || !met || !win || !clk || !taps || !sc || !ns || !smp || !cm)
            return false;
        L.blob_size           = h->arena_bytes;
        L.in_ring_offset      = in->offset;   L.in_ring_size      = in->bytes;
        L.out_ring_offset     = out->offset;  L.out_ring_size     = out->bytes;
        L.nrt_out_ring_offset = nrt->offset;  L.nrt_out_ring_size = nrt->bytes;
        L.control_offset      = ctl->offset;  L.control_bytes     = ctl->bytes;
        L.metrics_offset      = met->offset;  L.metrics_bytes     = met->geom[CLOCKWORK_GEOM_METRICS_FIELDS] * 4u;
        L.window_offset       = win->offset;  L.window_bytes      = win->bytes;
        L.clock_state_offset  = clk->offset;  L.clock_state_bytes = clk->bytes;
        L.audio_offset        = taps->offset;
        L.audio_slot_count    = taps->geom[CLOCKWORK_GEOM_TAPS_SLOTS];
        L.audio_slot_bytes    = taps->geom[CLOCKWORK_GEOM_TAPS_SLOT_BYTES];
        L.audio_header_bytes  = taps->geom[CLOCKWORK_GEOM_TAPS_HEADER_BYTES];
        L.audio_frames        = taps->geom[CLOCKWORK_GEOM_TAPS_FRAMES];
        L.audio_channels      = taps->geom[CLOCKWORK_GEOM_TAPS_CHANNELS];
        L.audio_sample_rate   = taps->geom[CLOCKWORK_GEOM_TAPS_SAMPLE_RATE];
        L.scope_offset        = sc->offset;
        L.scope_max           = sc->geom[CLOCKWORK_GEOM_SCOPE_SLOTS];
        L.scope_header_bytes  = sc->geom[CLOCKWORK_GEOM_SCOPE_HEADER_BYTES];
        L.scope_slot_bytes    = sc->geom[CLOCKWORK_GEOM_SCOPE_SLOT_BYTES];
        L.scope_slot_header   = sc->geom[CLOCKWORK_GEOM_SCOPE_SLOT_HEADER];
        L.scope_ring_frames   = sc->geom[CLOCKWORK_GEOM_SCOPE_RING_FRAMES];
        L.scope_channels      = sc->geom[CLOCKWORK_GEOM_SCOPE_CHANNELS];
        // Optional: an arena with no track taps has no entry, and that is
        // zero slots, not a refusal.
        if (const auto* tr = clockwork_arena_find(h, CLOCKWORK_ARENA_TRACK_TAPS)) {
            L.track_offset = tr->offset;
            L.track_slots  = tr->geom[CLOCKWORK_GEOM_TRACK_SLOTS];
            L.track_first  = tr->geom[CLOCKWORK_GEOM_TRACK_FIRST_INDEX];
        }
        L.native_stats_offset = ns->offset;   L.native_stats_bytes = ns->bytes;
        L.sample_clock_offset = smp->offset;  L.sample_clock_bytes = smp->bytes;
        L.channel_map_offset  = cm->offset;   L.channel_map_bytes  = cm->bytes;
        return L.check(why);
    }

    // Scope-stream slot `index` in ONE index space: the guest's slots first,
    // then the engine's track taps. nullptr past the end of both.
    uint8_t* scopeSlotAt(uint8_t* base, uint32_t index) const {
        if (index < scope_max)
            return base + scope_offset + scope_header_bytes + size_t(index) * scope_slot_bytes;
        const uint32_t t = index - scope_max;
        if (t < track_slots)
            return base + track_offset + size_t(t) * scope_slot_bytes;
        return nullptr;
    }
    uint32_t scopeSlotCount() const { return scope_max + track_slots; }

    // True when a reader built from THIS tree can address the segment the
    // table describes. `why` names the first failed check, for a log line.
    bool check(const char** why) const {
        const auto fail = [why](const char* w) { if (why) *why = w; return false; };
        // 64-bit so a hostile count times a hostile stride cannot wrap.
        const auto inside = [this](uint64_t off, uint64_t len) {
            return off <= blob_size && len <= blob_size - off;
        };
        if (blob_size == 0) return fail("empty blob");
        if (in_ring_size == 0 || !inside(in_ring_offset, in_ring_size)) return fail("ingress ring outside the blob");
        if (out_ring_size == 0 || !inside(out_ring_offset, out_ring_size)) return fail("egress ring outside the blob");
        if (nrt_out_ring_size == 0 || !inside(nrt_out_ring_offset, nrt_out_ring_size)) return fail("NRT egress ring outside the blob");
        if (control_bytes < sizeof(ControlPointers) || !inside(control_offset, control_bytes)) return fail("control block too small or outside the blob");
        if (metrics_bytes < sizeof(PerformanceMetrics) || !inside(metrics_offset, metrics_bytes)) return fail("metrics smaller than this reader's PerformanceMetrics, or outside the blob");
        if (!inside(window_offset, window_bytes)) return fail("window outside the blob");
        if (clock_state_bytes < sizeof(ClockworkClockState) || !inside(clock_state_offset, clock_state_bytes)) return fail("clock state too small or outside the blob");
        if (audio_header_bytes != SHM_AUDIO_HEADER_SIZE) return fail("audio tap slot shape differs from this reader's");
        if (audio_channels > SHM_AUDIO_CHANNELS) return fail("audio taps wider than this reader can interleave");
        if (audio_slot_count < SHM_AUDIO_SLOTS) return fail("arena has fewer audio taps than out and in");
        if (audio_slot_bytes < uint64_t(SHM_AUDIO_HEADER_SIZE) + uint64_t(audio_frames) * audio_channels * sizeof(float)) return fail("audio tap slot smaller than its declared ring");
        if (!inside(audio_offset, uint64_t(audio_slot_count) * audio_slot_bytes)) return fail("audio taps outside the blob");
        if (scope_slot_header != SHM_SCOPE_STREAM_HEADER_SIZE || scope_channels != SHM_SCOPE_STREAM_CHANNELS) return fail("scope slot shape differs from this reader's");
        if (scope_slot_bytes < uint64_t(SHM_SCOPE_STREAM_HEADER_SIZE) + uint64_t(scope_ring_frames) * scope_channels * sizeof(float)) return fail("scope slot smaller than its declared ring");
        if (!inside(scope_offset, uint64_t(scope_header_bytes) + uint64_t(scope_max) * scope_slot_bytes)) return fail("scope streams outside the blob");
        if (track_slots != 0) {
            if (!inside(track_offset, uint64_t(track_slots) * scope_slot_bytes)) return fail("track taps outside the blob");
            if (track_first != scope_max) return fail("track taps are not numbered after the scope slots");
        }
        if (native_stats_bytes < NATIVE_STATS_SIZE || !inside(native_stats_offset, native_stats_bytes)) return fail("native stats too small or outside the blob");
        if (sample_clock_bytes < SAMPLE_CLOCK_SIZE || !inside(sample_clock_offset, sample_clock_bytes)) return fail("sample clock too small or outside the blob");
        if (channel_map_bytes < sizeof(ClockworkChannelMapState) || !inside(channel_map_offset, channel_map_bytes)) return fail("channel map too small or outside the blob");
        return true;
    }
};

// ── Writing the table ───────────────────────────────────────────────────────
//
// Whoever creates an arena writes its header from these constants before
// anything else is placed in it: init_memory for the engine's own arena, the
// segment creator for a mapped one (both write it; the bytes are the same).
// `state` is stored last, behind a release fence, so a reader that sees it
// published and acquires sees the whole table.
inline void writeArenaHeader(uint8_t* base, uint32_t instance_id = 0) {
    auto* h = reinterpret_cast<ClockworkArenaHeader*>(base);
    std::memset(static_cast<void*>(h), 0, sizeof *h);
    h->magic        = CLOCKWORK_ARENA_MAGIC;
    h->version      = CLOCKWORK_ARENA_VERSION;
    h->header_bytes = ARENA_HEADER_SIZE;
    h->instance_id  = instance_id;
    h->arena_bytes  = TOTAL_BUFFER_SIZE;
    h->block_bytes  = CLOCKWORK_BLOCK_SIZE;
    h->guest_offset = GUEST_REGION_START;
    h->guest_bytes  = GUEST_REGION_SIZE;
    h->entry_bytes  = sizeof(ClockworkArenaEntry);
    uint32_t n = 0;
    const auto put = [&](uint32_t id, uint32_t offset, uint32_t bytes, uint32_t owner,
                         std::initializer_list<uint32_t> geom = {}) {
        ClockworkArenaEntry& e = h->entries[n++];
        e.id = id; e.offset = offset; e.bytes = bytes; e.owner = owner;
        uint32_t i = 0;
        for (uint32_t g : geom) e.geom[i++] = g;
    };
    put(CLOCKWORK_ARENA_CONTROL,         CONTROL_START,         CONTROL_SIZE,         CLOCKWORK_OWNER_CLOCKWORK);
    put(CLOCKWORK_ARENA_METRICS,         METRICS_START,         METRICS_SIZE,         CLOCKWORK_OWNER_CLOCKWORK, { METRICS_SIZE / 4u });
    put(CLOCKWORK_ARENA_NATIVE_STATS,    NATIVE_STATS_START,    NATIVE_STATS_SIZE,    CLOCKWORK_OWNER_CLOCKWORK);
    put(CLOCKWORK_ARENA_CLOCK_ANCHORS,   CLOCK_ANCHORS_START,   CLOCK_ANCHORS_SIZE,   CLOCKWORK_OWNER_HOST,
        { NTP_START_TIME_START - CLOCK_ANCHORS_START, DRIFT_OFFSET_START - CLOCK_ANCHORS_START,
          GLOBAL_OFFSET_START - CLOCK_ANCHORS_START });
    put(CLOCKWORK_ARENA_CLOCK_STATE,     CLOCK_STATE_START,     CLOCK_STATE_SIZE,     CLOCKWORK_OWNER_CLOCKWORK);
    put(CLOCKWORK_ARENA_SAMPLE_CLOCK,    SAMPLE_CLOCK_START,    SAMPLE_CLOCK_SIZE,    CLOCKWORK_OWNER_CLOCKWORK);
    put(CLOCKWORK_ARENA_CHANNEL_MAP,     CHANNEL_MAP_START,     CHANNEL_MAP_SIZE,     CLOCKWORK_OWNER_CLOCKWORK);
    put(CLOCKWORK_ARENA_NODE_ID_COUNTER, NODE_ID_COUNTER_START, NODE_ID_COUNTER_SIZE, CLOCKWORK_OWNER_CLOCKWORK);
    put(CLOCKWORK_ARENA_IN_RING,         IN_BUFFER_START,       IN_BUFFER_SIZE,       CLOCKWORK_OWNER_CLIENT,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)) });
    put(CLOCKWORK_ARENA_OUT_RING,        OUT_BUFFER_START,      OUT_BUFFER_SIZE,      CLOCKWORK_OWNER_CLOCKWORK,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)) });
    put(CLOCKWORK_ARENA_NRT_OUT_RING,    NRT_OUT_BUFFER_START,  NRT_OUT_BUFFER_SIZE,  CLOCKWORK_OWNER_CLOCKWORK,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)) });
    put(CLOCKWORK_ARENA_AUDIO_TAPS,      SHM_AUDIO_START,       SHM_AUDIO_TOTAL_SIZE, CLOCKWORK_OWNER_CLOCKWORK,
        { SHM_AUDIO_SLOTS, SHM_AUDIO_SLOT_SIZE, SHM_AUDIO_HEADER_SIZE, SHM_AUDIO_FRAMES,
          SHM_AUDIO_CHANNELS, SHM_AUDIO_SAMPLE_RATE });
    put(CLOCKWORK_ARENA_TRACK_TAPS,      SHM_TRACK_TAPS_START,  SHM_TRACK_TAPS_SIZE,  CLOCKWORK_OWNER_CLOCKWORK,
        { SHM_TRACK_TAPS_SLOTS, SHM_SCOPE_SLOT_SIZE, SHM_SCOPE_SLOT_HEADER_SIZE, SHM_SCOPE_RING_FRAMES,
          SHM_SCOPE_CHANNELS, SHM_SCOPE_TRACK_SLOT_BASE });
    put(CLOCKWORK_ARENA_CLIENT_SLOTS,    CLIENT_SLOTS_START,    CLIENT_SLOTS_SIZE,    CLOCKWORK_OWNER_CLIENT,
        { CLIENT_SLOT_COUNT, CLIENT_SLOT_SIZE, CLIENT_SLOTS_HEADER_SIZE, CLIENT_SLOT_STRUCTS_OFFSET,
          CLIENT_SLOT_STRUCTS_SIZE, CLIENT_SLOT_STACK_OFFSET, CLIENT_SLOT_STACK_SIZE });
    put(CLOCKWORK_ARENA_GUEST_CONFIG,    GUEST_CONFIG_START,    GUEST_CONFIG_SIZE,    CLOCKWORK_OWNER_HOST);
    put(CLOCKWORK_ARENA_GUEST_WINDOW,    SHM_WINDOW_START,      SHM_WINDOW_SIZE,      CLOCKWORK_OWNER_GUEST);
    put(CLOCKWORK_ARENA_SCOPE,           SHM_SCOPE_START,       SHM_SCOPE_TOTAL_SIZE, CLOCKWORK_OWNER_GUEST,
        { SHM_SCOPE_MAX_SCOPES, SHM_SCOPE_HEADER_SIZE, SHM_SCOPE_SLOT_SIZE, SHM_SCOPE_SLOT_HEADER_SIZE,
          SHM_SCOPE_RING_FRAMES, SHM_SCOPE_CHANNELS });
    put(CLOCKWORK_ARENA_GUEST_PERSIST,   GUEST_PERSIST_START,   GUEST_PERSIST_SIZE,   CLOCKWORK_OWNER_GUEST);
    h->entry_count = n;
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<uint32_t>*>(&h->state)->store(CLOCKWORK_ARENA_PUBLISHED,
                                                              std::memory_order_release);
}

// The header at the front of an arena.
inline const ClockworkArenaHeader* arenaHeader(const void* base) {
    return reinterpret_cast<const ClockworkArenaHeader*>(base);
}

// ─── SAB layout cross-language assertions ──────────────────────────────────
// JS reads these structs via hand-mirrored offset constants in
// js/lib/*. Encode the expected indices here so the build fails if a
// C++ field moves without the JS mirror following. One-directional —
// editing the JS file alone won't trip these.
//
// offsetof on classes containing std::atomic<integral> is conditionally
// supported by C++17/20 but accepted by clang/gcc/MSVC; suppress the
// pedantic warning around the block.

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

#define CLOCKWORK_ASSERT_OFFSET(StructName, field, expectedBytes, jsRef)            \
    static_assert(offsetof(StructName, field) == (expectedBytes),            \
                  #StructName "::" #field " offset drifted from " jsRef)

#define CLOCKWORK_ASSERT_METRIC(field, jsIdx)                                       \
    CLOCKWORK_ASSERT_OFFSET(PerformanceMetrics, field,                              \
                     (jsIdx) * sizeof(uint32_t),                             \
                     "js/lib/metrics_offsets.js")

// ClockworkClockState ↔ js/lib/clockwork_clock_protocol.js
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, bpm,               0,
                 "js/lib/clockwork_clock_protocol.js SC_BPM_I64");
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, beat_origin_ntp,   8,
                 "js/lib/clockwork_clock_protocol.js SC_BEAT_ORIGIN_NTP_I64");
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, is_playing_at_ntp, 16,
                 "js/lib/clockwork_clock_protocol.js SC_IS_PLAYING_AT_NTP_I64");
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, is_playing,        24,
                 "js/lib/clockwork_clock_protocol.js SC_IS_PLAYING_I32");
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, flags,             28,
                 "js/lib/clockwork_clock_protocol.js SC_FLAGS_I32");
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, meter,             32,
                 "js/lib/clockwork_clock_protocol.js SC_METER_I32");

// ControlPointers ↔ js/lib/control_offsets.js (JS exports byte offsets directly)
CLOCKWORK_ASSERT_OFFSET(ControlPointers, in_head,        0,  "js/lib/control_offsets.js IN_HEAD");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, in_tail,        4,  "js/lib/control_offsets.js IN_TAIL");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, out_head,       8,  "js/lib/control_offsets.js OUT_HEAD");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, out_tail,       12, "js/lib/control_offsets.js OUT_TAIL");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, nrt_out_head,     16, "js/lib/control_offsets.js NRT_OUT_HEAD");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, nrt_out_tail,     20, "js/lib/control_offsets.js NRT_OUT_TAIL");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, in_sequence,    24, "js/lib/control_offsets.js IN_SEQUENCE");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, out_sequence,   28, "js/lib/control_offsets.js OUT_SEQUENCE");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, nrt_out_sequence, 32, "js/lib/control_offsets.js NRT_OUT_SEQUENCE");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, status_flags,   36, "js/lib/control_offsets.js STATUS_FLAGS");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, in_write_lock,  40, "js/lib/control_offsets.js IN_WRITE_LOCK");
CLOCKWORK_ASSERT_OFFSET(ControlPointers, _padding,       44, "js/lib/control_offsets.js padding");

// PerformanceMetrics ↔ js/lib/metrics_offsets.js (all fields uint32; JS uses array index)
CLOCKWORK_ASSERT_METRIC(process_count,                   0);
CLOCKWORK_ASSERT_METRIC(messages_processed,              1);
CLOCKWORK_ASSERT_METRIC(messages_dropped,                2);
CLOCKWORK_ASSERT_METRIC(scheduler_queue_depth,           3);
CLOCKWORK_ASSERT_METRIC(scheduler_queue_max,             4);
CLOCKWORK_ASSERT_METRIC(scheduler_queue_dropped,         5);
CLOCKWORK_ASSERT_METRIC(messages_sequence_gaps,          6);
CLOCKWORK_ASSERT_METRIC(wasm_errors,                     7);
CLOCKWORK_ASSERT_METRIC(scheduler_lates,                 8);
CLOCKWORK_ASSERT_METRIC(osc_out_messages_sent,           9);
CLOCKWORK_ASSERT_METRIC(osc_out_bytes_sent,              10);
CLOCKWORK_ASSERT_METRIC(osc_in_messages_received,        11);
CLOCKWORK_ASSERT_METRIC(osc_in_bytes_received,           12);
CLOCKWORK_ASSERT_METRIC(osc_in_dropped_messages,         13);
CLOCKWORK_ASSERT_METRIC(osc_in_corrupted,                14);
CLOCKWORK_ASSERT_METRIC(debug_messages_received,         15);
CLOCKWORK_ASSERT_METRIC(debug_bytes_received,            16);
CLOCKWORK_ASSERT_METRIC(in_buffer_used_bytes,            17);
CLOCKWORK_ASSERT_METRIC(out_buffer_used_bytes,           18);
CLOCKWORK_ASSERT_METRIC(nrt_out_buffer_used_bytes,         19);
CLOCKWORK_ASSERT_METRIC(in_buffer_peak_bytes,            20);
CLOCKWORK_ASSERT_METRIC(out_buffer_peak_bytes,           21);
CLOCKWORK_ASSERT_METRIC(nrt_out_buffer_peak_bytes,         22);
CLOCKWORK_ASSERT_METRIC(scheduler_max_late_ms,           23);
CLOCKWORK_ASSERT_METRIC(scheduler_last_late_ms,          24);
CLOCKWORK_ASSERT_METRIC(scheduler_last_late_tick,        25);
CLOCKWORK_ASSERT_METRIC(ring_buffer_direct_write_fails,  26);
// Link [27-38] is native-only and intentionally unasserted (the web merged
// array never reads those slots). The cross-platform system-info block [39-49]
// is written by shared C++ on every runtime and IS asserted against the JS
// mirror in js/lib/metrics_offsets.js.
CLOCKWORK_ASSERT_METRIC(clockwork_version_major,        39);
CLOCKWORK_ASSERT_METRIC(clockwork_version_minor,        40);
CLOCKWORK_ASSERT_METRIC(clockwork_version_patch,        41);
CLOCKWORK_ASSERT_METRIC(audio_sample_rate,               42);
CLOCKWORK_ASSERT_METRIC(audio_block_size,                43);
CLOCKWORK_ASSERT_METRIC(audio_output_channels,           44);
CLOCKWORK_ASSERT_METRIC(audio_input_channels,            45);
CLOCKWORK_ASSERT_METRIC(clock_tempo_mbpm,                46);
CLOCKWORK_ASSERT_METRIC(clock_beat_centi,                47);
CLOCKWORK_ASSERT_METRIC(clock_phase_centi,               48);
CLOCKWORK_ASSERT_METRIC(clock_playing,                   49);
CLOCKWORK_ASSERT_METRIC(_metrics_reserved,               50);

// METRICS_SIZE must cover the whole struct and stay a multiple of 8: the arena
// regions that follow (NTP time, ClockworkClockState) are 8-byte aligned and read
// via Float64/BigInt64 views. _metrics_reserved exists solely to satisfy this;
// these asserts make removing it (or any odd field count) a build error.
static_assert(sizeof(PerformanceMetrics) == METRICS_SIZE,
              "METRICS_SIZE must equal sizeof(PerformanceMetrics)");
static_assert(METRICS_SIZE % 8 == 0,
              "METRICS_SIZE must be a multiple of 8 to keep following regions 8-byte aligned");

#undef CLOCKWORK_ASSERT_METRIC
#undef CLOCKWORK_ASSERT_OFFSET

#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#endif // CLOCKWORK_SHARED_MEMORY_H
