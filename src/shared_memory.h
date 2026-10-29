// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#ifndef CLOCKWORK_SHARED_MEMORY_H
#define CLOCKWORK_SHARED_MEMORY_H

#include "clock/clock_math.h"   // beatAt, for ClockworkClockSnapshot
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "memory_profile.h"
#include "clockwork_arena.h"   // the table every reader takes the layout from
#include "ring/ring.h"   // Message, MESSAGE_MAGIC, PADDING_MAGIC
#include "shm_audio_buffer.hpp"
#include "shm_tap_stream.hpp"

// The bit-pattern helpers, the default tempo and the session clock's state
// live in clockwork_clock_state.h: the one header a DSP includes to read
// DspConfig::clock, so it must not depend on the layout below.
#include "clockwork_clock_state.h"
#include "metrics_history.h"      // the history region's size (kRegionBytes)

// ============================================================================
// BUFFER LAYOUT CONFIGURATION
// ============================================================================
// Memory layout constants for shared ring buffer.
// NOTE: These are RELATIVE OFFSETS from the ring buffer base address.
// Actual addresses are calculated as: base_address + offset
// On WASM the base is the linker's static buffer (data segment), retrieved via
// get_arena_base() at runtime; every other runtime takes it from
// get_shared_memory_base() — the local arena, or an external segment when one
// was supplied.
//
// TO MODIFY BUFFER SIZES: Change the SIZE constants below.
// All offsets are calculated automatically using constexpr.
// ============================================================================

// User-configurable buffer sizes.
// Balanced to prevent message drops in the audioworklet; sized per device via
// memory_profile.h (defaults: IN 768KB, OUT 128KB, NRT-out 64KB).
constexpr uint32_t IN_RING_SIZE     = CLOCKWORK_IN_RING_SIZE;    // OSC messages from host to the engine (large for bulk payloads)
constexpr uint32_t RT_OUT_RING_SIZE    = CLOCKWORK_RT_OUT_RING_SIZE;   // OSC replies from the engine to host (prevent drops)
constexpr uint32_t NRT_OUT_RING_SIZE  = CLOCKWORK_NRT_OUT_RING_SIZE; // NRT-thread egress ring (replies, notifications, debug)
constexpr uint32_t RING_CURSORS_SIZE       = 56;    // Atomic control pointers & flags (13 fields × 4 bytes + 4 padding for 8-byte alignment)
// Clockwork's own control verbs, written at the boundary and drained by the
// control pass with no audio block in the loop (docs/RESTARTS.md, "The
// control ring"): its cursors first (ControlRingCursors, one line), then
// the ring. Small: a control verb is a few hundred bytes and answered at
// once; a bulk payload is the guest's and rides the in ring.
constexpr uint32_t CONTROL_RING_HEADER_SIZE = 64;
constexpr uint32_t CONTROL_RING_SIZE        = 64u * 1024u;   // cursors + ring
constexpr uint32_t METRICS_SIZE       = 320;   // Performance metrics: 80 fields * 4 bytes = 320 bytes (multiple of 8); 66 in use
constexpr uint32_t GLOBAL_OFFSET_SIZE = 4;     // Global timing offset in milliseconds (int32, atomic) - for multi-system sync (Ableton Link, NTP, etc.)
constexpr uint32_t CLOCK_STATE_SIZE = 40; // ClockworkClock session state: 3 atomic uint64 + 3 atomic uint32 + 4 padding = 40 bytes

// Node tree mirror configuration (for observing synth/group hierarchy via polling)
// A window of shared memory the guest may publish through, reached as
// DspConfig::shm_window. Clockwork reserves it, hands over base and length,
// and never reads a byte: what goes in it, and in what shape, is the guest's.
//
// Sized per device via memory_profile.h. A guest that publishes nothing simply
// leaves it alone.
constexpr uint32_t GUEST_WINDOW_SIZE = CLOCKWORK_WINDOW_BYTES;

// The scope: two shm_audio_buffer slots, out and in, written by
// clockwork at the device edge inside every tick on every host
// (shm_audio_buffer.hpp). Per-slot ring duration is CLOCKWORK_SCOPE_SECONDS
// (1s in production, larger in test builds so captures don't wrap).

// Where each region sits is decided in one place — "The arena map" below —
// once every size is known.
constexpr uint32_t NODE_ID_COUNTER_SIZE  = 4;     // Int32, atomic — for nextNodeId() range allocation

// Guest config — bytes written by the HOST before the guest boots and read by
// the GUEST, with clockwork in between carrying them and nothing more. It
// copies whatever it is handed into this region (clockwork_init) and passes base and
// length on as DspConfig::guest_config. No clockwork code reads a field, so the
// layout is a private matter between a host and its guest: this repository's
// native host writes the name=value text of src/native/GuestConfigText.h, and
// the web one writes whatever the product's encodeGuestConfig() returns.
//
// The size is a ceiling, not a shape. A block that does not fit is refused
// loudly on both sides rather than truncated, because half a config block is a
// different config and the guest cannot tell.
constexpr uint32_t GUEST_CONFIG_SIZE  = 1024;  // bytes: room for a page of name=value lines

// Tap streams — per-slot lossless audio rings for visualisation.
// Protocol (struct layout, writer/reader) lives in
// shm_tap_stream.hpp; rationale in docs/PORTS.md.
// The stream's producer appends every block; readers combine a slot's cursor with the
// sample-clock region (below) and their own wall clock to display exactly what
// the listener is hearing. TAPS_SLOTS / TAP_RING_FRAMES sized
// per device via memory_profile.h: 32 slots for the guest, then the track-tap
// slots the ENGINE writes natively (TrackControl.cpp), one region and one
// index space (tapSlotAt below).
constexpr uint32_t TAP_CHANNELS = TAP_STREAM_CHANNELS;
constexpr uint32_t TAPS_HEADER_SIZE = 32;  // Global header (16-aligned)
constexpr uint32_t TAP_SLOT_HEADER_SIZE = TAP_STREAM_HEADER_SIZE;
constexpr uint32_t TAP_SLOT_SIZE = TAP_STREAM_SLOT_SIZE;
// One tap region: the header, the guest's TAPS_SLOTS slots, then the engine's
// TAP_TRACK_SLOTS track slots (memory_profile.h), every slot the same shape,
// one index space numbered from 0 with the guest's first. The table carries
// the geometry (CLOCKWORK_GEOM_TAP_*); no reader needs these constants.
constexpr uint32_t TAPS_SIZE = TAPS_HEADER_SIZE + ((TAPS_SLOTS + TAP_TRACK_SLOTS) * TAP_SLOT_SIZE);


// Tap header layout (at TAPS_START):
//   [0..3]   u32  maxTaps
//   [4..7]   u32  activeCount
//   [8..11]  u32  ringFrames (per-slot capacity)
//   [12..15] u32  version (increments on tap add/remove)
//   [16..31] reserved
//
// Per-tap slot: a shm_tap_stream (see shm_tap_stream.hpp).

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
// tap streams, plus anything needing audible-time alignment (recording
// markers, visual sync). See docs/PORTS.md.
//
// Beside it, outside the seqlock: when the last block was rendered, by the
// host's clocks as it handed the block over — its wall clock (LAST_WALL), and
// its monotonic clock where it has one (LAST_MONO: native, an embedded host;
// a worklet has none) — the one record that the audio was away: a stall, a
// page's context suspended, a worklet gone. It survives a new worklet over
// the same memory or an embedded host attaching again, so the first block
// back on any target sees how long it was (clockwork_audio_back): by the
// monotonic clock where there is one, which a system clock set by hand
// cannot fool, else by the wall.
constexpr uint32_t SAMPLE_CLOCK_SIZE  = 48;
// Field byte offsets within the sample-clock region.
constexpr uint32_t SAMPLE_CLOCK_SEQ            = 0;   // u32 seqlock (odd = mid-update)
constexpr uint32_t SAMPLE_CLOCK_SAMPLE_RATE    = 4;   // u32
constexpr uint32_t SAMPLE_CLOCK_ENGINE_FRAMES  = 8;   // u64 engine frames at block start
constexpr uint32_t SAMPLE_CLOCK_DAC_NTP        = 16;  // f64 NTP seconds when that frame hits the DAC
constexpr uint32_t SAMPLE_CLOCK_OUT_LATENCY    = 24;  // u32 device output latency, frames
constexpr uint32_t SAMPLE_CLOCK_LAST_WALL      = 32;  // f64 wall NTP of the last block rendered (0: none yet)
constexpr uint32_t SAMPLE_CLOCK_LAST_MONO      = 40;  // f64 the host's monotonic clock at it, seconds (0: the host has none)
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
// ── The sample clock, and the window end every tap consumer wants ─────────
//
// Here rather than with the segment because it is pure arithmetic over the
// offsets above and a tap reader: no mapping, no POSIX, nothing a browser
// build cannot have. shm_segment.hpp re-exports it.
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

    // Slot-local cursor of the newest AUDIBLE sample of a tap stream —
    // the canonical window end for every tap consumer. Clamped to the
    // stream's write position; a mapping that lands before the stream's
    // anchor (engine counter reset, wall-clock step) yields 0 so consumers
    // show silence rather than not-yet-heard audio. Falls back to the raw
    // write position when this view is invalid (no publisher yet:
    // headless engines, older builds).
    uint64_t audible_end(const shm_tap_stream_reader& r, double now_ntp) const {
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
    uint64_t audible_end(const shm_tap_stream_reader& r) const {
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

// The last block's wall time (SAMPLE_CLOCK_LAST_WALL): written by the audio
// thread as it renders, read by the first block back on whichever host
// renders next. Outside the seqlock: one word, one writer at a time.
inline double read_last_block_wall(const uint8_t* tl) {
    if (!tl) return 0.0;
    return clockwork::bitsToDouble(
        reinterpret_cast<const std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_LAST_WALL)->load(std::memory_order_relaxed));
}
inline double read_last_block_mono(const uint8_t* tl) {
    if (!tl) return 0.0;
    return clockwork::bitsToDouble(
        reinterpret_cast<const std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_LAST_MONO)->load(std::memory_order_relaxed));
}
inline void write_last_block_time(uint8_t* tl, double wall_ntp, double monotonic_secs) {
    if (!tl) return;
    reinterpret_cast<std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_LAST_WALL)
        ->store(clockwork::doubleToBits(wall_ntp), std::memory_order_relaxed);
    reinterpret_cast<std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_LAST_MONO)
        ->store(clockwork::doubleToBits(monotonic_secs), std::memory_order_relaxed);
}

// The writer, once for every host that renders: odd seq, release fence
// (orders the odd store before the field stores for any reader that sees
// them), relaxed atomic field stores, even seq with release. The audio
// thread's only — a single writer by construction.
inline void write_sample_clock(uint8_t* tl, uint64_t engine_frames, uint32_t sample_rate,
                               double dac_ntp, uint32_t output_latency_frames) {
    if (!tl || sample_rate == 0) return;
    auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(tl + SAMPLE_CLOCK_SEQ);
    auto* sr  = reinterpret_cast<std::atomic<uint32_t>*>(tl + SAMPLE_CLOCK_SAMPLE_RATE);
    auto* fr  = reinterpret_cast<std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_ENGINE_FRAMES);
    auto* nb  = reinterpret_cast<std::atomic<uint64_t>*>(tl + SAMPLE_CLOCK_DAC_NTP);
    auto* lat = reinterpret_cast<std::atomic<uint32_t>*>(tl + SAMPLE_CLOCK_OUT_LATENCY);
    const uint32_t s = seq->load(std::memory_order_relaxed);
    seq->store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    sr->store(sample_rate, std::memory_order_relaxed);
    fr->store(engine_frames, std::memory_order_relaxed);
    nb->store(clockwork::doubleToBits(dac_ntp), std::memory_order_relaxed);
    lat->store(output_latency_frames, std::memory_order_relaxed);
    seq->store(s + 2, std::memory_order_release);
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
// both everywhere is expensive.
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
    return (kind & 0xFFu) | ((slotPlusOne & 0xFFFFu) << 8u);
}
constexpr uint32_t channelKind(uint32_t w)   { return w & 0xFFu; }
constexpr uint32_t channelSlot(uint32_t w)   { return (w >> 8u) & 0xFFFFu; }  // 0 = none

struct ClockworkStreamEntry {
    uint32_t port;        // ClockworkPort; 0 when the slot is free
    uint32_t direction;   // kClockworkPortSource (1) / kClockworkPortSink (2)
    uint32_t first;       // first channel of the range
    uint32_t count;       // how many
    std::array<char, CHANNEL_MAP_NAME_BYTES> name;   // NUL-terminated, truncated to fit
};

struct alignas(16) ClockworkChannelMapState {
    std::atomic<uint32_t> generation;    // odd while the cold half is written
    std::atomic<uint32_t> device_in;     // device input channels: [0, device_in)
    std::atomic<uint32_t> device_out;
    std::atomic<uint32_t> max_channels;  // how much of in/out is meaningful

    // Hot: indexed by channel, read at the point of use.
    std::array<std::atomic<uint32_t>, CLOCKWORK_MAX_CHANNELS> in;
    std::array<std::atomic<uint32_t>, CLOCKWORK_MAX_CHANNELS> out;

    // Cold: guarded by `generation`.
    std::array<ClockworkStreamEntry, CHANNEL_MAP_MAX_STREAMS> streams;
};

constexpr uint32_t CHANNEL_MAP_SIZE  = sizeof(ClockworkChannelMapState);

// ============================================================================
// THE ARENA MAP
// ============================================================================
// Every region's place, in one list, in the order it lies in memory. Two
// halves (clockwork_arena.h says why): the CLOCKWORK BLOCK first — the
// header, the small engine state, the rings, the taps — and
// the GUEST REGION after it. Nothing here is read by anyone but the engine
// and its tests: readers take the table the header carries (writeArenaHeader
// below), which is filled from these.
//
// The order is chosen, not accumulated. Each half is two contiguous runs
// (clockwork_arena.h, "Audiences"): first what is PUBLISHED — the data
// contracts a client reads by hand — then what is not: the block's transport
// (the ring cursors, the rings, the counter), the guest's
// own (its config, its persistent bytes). Within a run, small state sits
// together at the front and the large arrays last, each 16-aligned because
// their slot structs are alignas(16). The header carries where each run ends.
constexpr uint32_t alignArena16(uint32_t v) { return (v + 15u) & ~15u; }
constexpr uint32_t alignArena8 (uint32_t v) { return (v + 7u)  & ~7u;  }

// ── clockwork's half ──
//   clockwork-state: public state any client reads
constexpr uint32_t ARENA_HEADER_START    = 0;
constexpr uint32_t ARENA_HEADER_SIZE     = CLOCKWORK_ARENA_HEADER_BYTES;
constexpr uint32_t CLOCKWORK_STATE_START = ARENA_HEADER_START + ARENA_HEADER_SIZE;
constexpr uint32_t METRICS_START         = CLOCKWORK_STATE_START;
// What every counter and gauge did over the last hour (metrics_history.h).
constexpr uint32_t METRICS_HISTORY_START = alignArena16(METRICS_START + METRICS_SIZE);
constexpr uint32_t METRICS_HISTORY_SIZE  = clockwork::metrics_history::kRegionBytes;
constexpr uint32_t NATIVE_STATS_START    = alignArena16(METRICS_HISTORY_START + METRICS_HISTORY_SIZE);
// The host's clock anchors, as one region: the NTP start time (f64) then the
// two offsets.
constexpr uint32_t CLOCK_ANCHORS_START   = alignArena8(NATIVE_STATS_START + NATIVE_STATS_SIZE);
constexpr uint32_t GLOBAL_OFFSET_START   = CLOCK_ANCHORS_START;
constexpr uint32_t CLOCK_ANCHORS_SIZE    = GLOBAL_OFFSET_SIZE;   // the host's global offset, and nothing else: the clock anchors itself
constexpr uint32_t CLOCK_STATE_START     = alignArena8(CLOCK_ANCHORS_START + CLOCK_ANCHORS_SIZE);
constexpr uint32_t SAMPLE_CLOCK_START    = alignArena16(CLOCK_STATE_START + CLOCK_STATE_SIZE);
constexpr uint32_t CHANNEL_MAP_START     = alignArena16(SAMPLE_CLOCK_START + SAMPLE_CLOCK_SIZE);
constexpr uint32_t CLOCKWORK_STATE_END   = alignArena16(CHANNEL_MAP_START + CHANNEL_MAP_SIZE);
//   clockwork-audio: audio streams any client reads
constexpr uint32_t CLOCKWORK_AUDIO_START = CLOCKWORK_STATE_END;
constexpr uint32_t SCOPE_START           = CLOCKWORK_AUDIO_START;
constexpr uint32_t TAPS_START            = alignArena16(SCOPE_START + SCOPE_SIZE);
constexpr uint32_t CLOCKWORK_AUDIO_END   = alignArena16(TAPS_START + TAPS_SIZE);
//   clockwork-transport: messages, through the client code only
constexpr uint32_t CLOCKWORK_TRANSPORT_START = CLOCKWORK_AUDIO_END;
constexpr uint32_t RING_CURSORS_START    = CLOCKWORK_TRANSPORT_START;
constexpr uint32_t NODE_ID_COUNTER_START = RING_CURSORS_START + RING_CURSORS_SIZE;
constexpr uint32_t IN_RING_START         = alignArena16(NODE_ID_COUNTER_START + NODE_ID_COUNTER_SIZE);
constexpr uint32_t RT_OUT_RING_START     = IN_RING_START + IN_RING_SIZE;
constexpr uint32_t NRT_OUT_RING_START    = RT_OUT_RING_START + RT_OUT_RING_SIZE;
constexpr uint32_t CONTROL_RING_START    = alignArena16(NRT_OUT_RING_START + NRT_OUT_RING_SIZE);
constexpr uint32_t CLOCKWORK_TRANSPORT_END = alignArena16(CONTROL_RING_START + CONTROL_RING_SIZE);
constexpr uint32_t CLOCKWORK_HALF_SIZE   = CLOCKWORK_TRANSPORT_END;

// ── the guest's half ──
//   guest-public: the guest's window, in the guest's format
constexpr uint32_t GUEST_HALF_START      = CLOCKWORK_HALF_SIZE;
constexpr uint32_t GUEST_PUBLIC_START    = GUEST_HALF_START;
constexpr uint32_t GUEST_WINDOW_START    = GUEST_PUBLIC_START;
constexpr uint32_t GUEST_PUBLIC_END      = alignArena16(GUEST_WINDOW_START + GUEST_WINDOW_SIZE);
//   guest-private: the guest's config and persistent store
constexpr uint32_t GUEST_PRIVATE_START   = GUEST_PUBLIC_END;
constexpr uint32_t GUEST_CONFIG_START    = GUEST_PRIVATE_START;
constexpr uint32_t GUEST_PERSIST_START   = alignArena16(GUEST_CONFIG_START + GUEST_CONFIG_SIZE);
constexpr uint32_t GUEST_PRIVATE_END     = alignArena16(GUEST_PERSIST_START + GUEST_PERSIST_SIZE);
constexpr uint32_t ARENA_SIZE            = GUEST_PRIVATE_END;
constexpr uint32_t GUEST_HALF_SIZE       = ARENA_SIZE - GUEST_HALF_START;

static_assert(sizeof(ClockworkArenaHeader) <= ARENA_HEADER_SIZE, "the arena header outgrew its reservation");
static_assert(RING_CURSORS_START % 16 == 0, "the ring cursors must be 16-byte aligned");
static_assert(CLOCK_STATE_START % 8 == 0,   "ClockworkClockState holds 64-bit atomics");
static_assert(SAMPLE_CLOCK_START % 16 == 0, "the sample clock is read as 64-bit words");
static_assert(CHANNEL_MAP_START % 16 == 0,  "ClockworkChannelMapState is alignas(16)");
static_assert(IN_RING_START % 16 == 0,      "rings start 16-aligned");
static_assert(SCOPE_START % 16 == 0,        "shm_audio_buffer is alignas(16)");
static_assert(TAPS_START % 16 == 0,         "shm_tap_stream is alignas(16)");
static_assert(GUEST_HALF_START % 16 == 0,   "the guest's half starts 16-aligned");
static_assert(CLOCKWORK_STATE_END <= CLOCKWORK_AUDIO_START && CLOCKWORK_AUDIO_END <= CLOCKWORK_TRANSPORT_START
              && CLOCKWORK_TRANSPORT_END == GUEST_PUBLIC_START && GUEST_PUBLIC_END <= GUEST_PRIVATE_START
              && GUEST_PRIVATE_END == ARENA_SIZE, "the five areas lie end to end and cover the arena");
static_assert(GUEST_WINDOW_START % 16 == 0,   "the guest window starts 16-aligned");
static_assert((TAPS_START + TAPS_HEADER_SIZE) % 16 == 0,
              "tap slots must be 16-byte aligned for shm_tap_stream");
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

// Control pointers structure (4-byte aligned for atomics, padded to 56 bytes for 8-byte alignment)
struct alignas(4) RingCursors {
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
    // Where a poll has read each egress ring to. Poll hands out the frames
    // where they lie, so the tail — what the writer measures free space
    // against — follows this one poll late: a batch's space goes back when
    // the next poll takes the read cursor as the tail. Reset with the rings.
    std::atomic<int32_t> out_read;
    std::atomic<int32_t> nrt_out_read;
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
    std::atomic<uint32_t> messages_dropped;        // 2: Messages refused on the way in (the IN ring full, or too big for it)
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
    std::atomic<uint32_t> link_audio_underruns;    // 34: Link Audio input frames that arrived too late to play
    std::atomic<uint32_t> link_audio_buffered_ms;  // 35: Link Audio held ahead of playback, the most of any input (ms)
    std::atomic<int32_t>  link_audio_drift_ppm;    // 36: read-rate deviation from 1.0 (ppm, signed)
    std::atomic<uint32_t> link_audio_publish;      // 37: publishing enabled 0/1
    std::atomic<uint32_t> link_audio_sinks;        // 38: active output sinks

    // ─── System info (cross-platform; written by shared C++ on every runtime) ─
    // Engine identity + audio connection details. Written once at init by
    // init_memory() (audio_processor.cpp). Constant for the session.
    std::atomic<uint32_t> clockwork_commit;        // 39: CLOCKWORK_COMMIT_WORD, the commit's first 8 hex digits (0: unknown)
    std::atomic<uint32_t> scheduler_capacity;      // 40: timed events the scheduler can hold (0: no scheduler)
    std::atomic<uint32_t> scheduler_pool_bytes;    // 41: bytes of timed messages the scheduler can hold (0: no scheduler)
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

    // The engine's clock against the host's wall clock, smoothed as the clock
    // is, in microseconds, signed (as uint32 bits): what the clock nudges by
    // (clock/TimeSource.h). Near 0 and steady is locked; a wander is the
    // smoothing not keeping up; a jump is a clock step or an anchor. Written
    // by publishClockMetrics on every target.
    std::atomic<uint32_t> clock_drift_us;           // 50: int32 bits
    std::atomic<uint32_t> _metrics_reserved2;       // 51: reserved (the page's clock offset in the merged array)
    // 52 is the client's (js/lib/metrics_offsets.js: the AudioContext state,
    // written into the merged array on the client side); the word is here so
    // the slots after it are one address space, and stays 0.
    std::atomic<uint32_t> _client_side;             // 52
    // The guest's pool and the rust heap, written at the metrics flush
    // (shared C++), slots 53-57.
    std::atomic<uint32_t> guest_pool_used_bytes;    // 53: bytes in use in the guest's pool now (its bulk memory)
    std::atomic<uint32_t> guest_pool_peak_bytes;    // 54: the most ever
    // Frames an egress ring refused, for want of room or for their size —
    // the RT ring (ring_buffer_write) or the NRT ring
    // (clockwork_egress_nrt_write): a reply, a broadcast or a log line the
    // clients never get. Zero in a
    // healthy engine; the line logging a refusal rides the same full ring, so
    // this count is the only trace.
    std::atomic<uint32_t> egress_frames_refused;    // 55
    std::atomic<uint32_t> rust_heap_used_bytes;     // 56: bytes in use in the RT router's block
    std::atomic<uint32_t> rust_heap_fallbacks;      // 57: Rust allocations that fell back to the system allocator
    // What the guest takes from malloc (src/guest_malloc.h): counted where the
    // link lets clockwork wrap the allocator (the web), 0 everywhere else.
    std::atomic<uint32_t> guest_malloc_bytes;       // 58: bytes the guest holds from malloc, taken inside its entry points
    std::atomic<uint32_t> guest_mallocs_processing; // 59: malloc/calloc/realloc calls inside dsp_process, dsp_osc or dsp_asset since boot
    // The whole program's mallocs after boot, any scope (src/guest_malloc.h):
    // what reaches the system allocator once the engine is up, which the
    // memory model says should be nothing. Counted where the link lets
    // clockwork wrap the allocator (the web), 0 everywhere else.
    std::atomic<uint32_t> mallocs_after_boot;       // 60: malloc/calloc/realloc calls since the engine was ready
    std::atomic<uint32_t> malloc_bytes_after_boot;  // 61: bytes taken since then and still held
    // Deliveries the transport could not make: a reply to a client that has
    // gone (a stream connection closed, or dropped for not reading), a
    // datagram the socket refused, a peer whose reply ring was full, or one
    // subscriber of a broadcast that any of these befell. The frame left its
    // egress ring and reached no one: the other half of what the clients
    // never got, beside egress_frames_refused. Counted by the gateway, one per
    // destination missed (OscEgress::dispatchEgress).
    std::atomic<uint32_t> egress_frames_undelivered; // 62
    // Times the engine's clock was anchored afresh since boot: its first
    // block, the audio back after being away, a new context — each a step
    // in the clock a reader would want to know about (clock/TimeSource.h).
    std::atomic<uint32_t> clock_anchors;             // 63
    // Timed events the scheduler dropped for coming due later than the host
    // allows (clockwork_set_timed_event_late_limit_ms; 0, the default, allows
    // any lateness and nothing is ever dropped for it). Not a want-of-room
    // drop: those are scheduler_queue_dropped.
    std::atomic<uint32_t> scheduler_late_dropped;    // 64
    // Cues the cue port's rate limit dropped (clockwork_osc_set_limit,
    // /clockwork/osc/cue-server/limit): a sender over its rate, or the port
    // over its own. 0 unless someone floods it.
    std::atomic<uint32_t> cues_dropped;              // 65
    std::atomic<uint32_t> reserved[14];              // 66-79: room for what comes next, 0
};

// The session clock — ClockworkClockState, its snapshot and readClockworkClock()
// — lives in clockwork_clock_state.h (included above). The size check stays
// here with the layout it belongs to.
static_assert(sizeof(ClockworkClockState) == CLOCK_STATE_SIZE,
              "ClockworkClockState size must match CLOCK_STATE_SIZE");

enum StatusFlags : uint32_t {
    STATUS_OK = 0,
    STATUS_BUFFER_FULL = 1u << 0u,
    STATUS_OVERRUN = 1u << 1u,
    STATUS_WASM_ERROR = 1u << 2u,
    STATUS_FRAGMENTED_MSG = 1u << 3u,
    // Whether there is a guest instance is not a flag: the arena's generation
    // word says it, odd between instances, and every door reads that
    // (in_ring_door.h, docs/RESTARTS.md).
    // The engine's control pass drains the control ring: a control verb of
    // clockwork's is written there by every boundary, where no audio block
    // is between it and its answer. Raised once the pass drains it, lowered
    // at shutdown; off in a build with no control pass (the worklet, the
    // lanes alone), where every verb rides the in ring.
    STATUS_CONTROL_SERVED = 1u << 5u
};

// The control ring's own cursors, at the front of its region
// (CONTROL_RING_START): the ring follows at CONTROL_RING_HEADER_SIZE. Its
// own and not RingCursors', so the region is one piece of lifetime arena —
// a rebuild resets the in ring's epoch and leaves this alone, which is why
// nothing of clockwork's control needs carrying across one.
struct alignas(16) ControlRingCursors {
    std::atomic<int32_t> head;
    std::atomic<int32_t> tail;
    std::atomic<int32_t> sequence;
    std::atomic<int32_t> write_lock;
    uint8_t              reserved[CONTROL_RING_HEADER_SIZE - 4 * sizeof(std::atomic<int32_t>)];
};
static_assert(sizeof(ControlRingCursors) == CONTROL_RING_HEADER_SIZE,
              "ControlRingCursors is the control ring's header line");

// Constants
// MAX_MESSAGE_SIZE bounds what a frame header on the INGRESS ring may CLAIM.
// It is not a bound for the egress rings and must not be used as one: it is
// derived from IN_RING_SIZE, which is six times RT_OUT_RING_SIZE and twelve
// times NRT_OUT_RING_SIZE by default, so a reader validating an egress frame
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
constexpr uint32_t MAX_MESSAGE_SIZE = IN_RING_SIZE - sizeof(Message);
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
    uint32_t control_ring_offset = 0, control_ring_size = 0;   // 0: an arena with none
    uint32_t control_offset = 0,      control_bytes = 0;
    uint32_t metrics_offset = 0,      metrics_bytes = 0;
    uint32_t metrics_history_offset = 0, metrics_history_bytes = 0;   // 0: an arena with none
    uint32_t window_offset = 0,       window_bytes = 0;
    uint32_t clock_state_offset = 0,  clock_state_bytes = 0;
    uint32_t audio_offset = 0, audio_slot_count = 0, audio_slot_bytes = 0;
    uint32_t audio_header_bytes = 0, audio_frames = 0, audio_channels = 0, audio_sample_rate = 0;
    uint32_t tap_offset = 0, tap_max = 0, tap_header_bytes = 0, tap_slot_bytes = 0;
    uint32_t tap_slot_header = 0, tap_ring_frames = 0, tap_channels = 0;
    // The engine's track-tap slots: the same shape, after the guest's in the
    // same region. Zero on a build that carves none.
    uint32_t tap_track_slots = 0;
    uint32_t native_stats_offset = 0, native_stats_bytes = 0;
    uint32_t sample_clock_offset = 0, sample_clock_bytes = 0;
    uint32_t channel_map_offset = 0,  channel_map_bytes = 0;

    // The engine's own table: what writeArenaHeader publishes.
    static ShmReaderLayout from_constants() {
        ShmReaderLayout L;
        L.blob_size           = ARENA_SIZE;
        L.in_ring_offset      = IN_RING_START;      L.in_ring_size      = IN_RING_SIZE;
        L.out_ring_offset     = RT_OUT_RING_START;     L.out_ring_size     = RT_OUT_RING_SIZE;
        L.nrt_out_ring_offset = NRT_OUT_RING_START; L.nrt_out_ring_size = NRT_OUT_RING_SIZE;
        L.control_ring_offset = CONTROL_RING_START; L.control_ring_size = CONTROL_RING_SIZE;
        L.control_offset      = RING_CURSORS_START;        L.control_bytes     = RING_CURSORS_SIZE;
        L.metrics_offset      = METRICS_START;        L.metrics_bytes     = METRICS_SIZE;
        L.metrics_history_offset = METRICS_HISTORY_START; L.metrics_history_bytes = METRICS_HISTORY_SIZE;
        L.window_offset       = GUEST_WINDOW_START;     L.window_bytes      = GUEST_WINDOW_SIZE;
        L.clock_state_offset  = CLOCK_STATE_START;    L.clock_state_bytes = CLOCK_STATE_SIZE;
        L.audio_offset        = SCOPE_START;
        L.audio_slot_count    = SCOPE_SLOTS;
        L.audio_slot_bytes    = SCOPE_SLOT_SIZE;
        L.audio_header_bytes  = SCOPE_HEADER_SIZE;
        L.audio_frames        = SCOPE_FRAMES;
        L.audio_channels      = SCOPE_CHANNELS;
        L.audio_sample_rate   = SCOPE_SAMPLE_RATE;
        L.tap_offset        = TAPS_START;
        L.tap_max           = TAPS_SLOTS;
        L.tap_header_bytes  = TAPS_HEADER_SIZE;
        L.tap_slot_bytes    = TAP_SLOT_SIZE;
        L.tap_slot_header   = TAP_SLOT_HEADER_SIZE;
        L.tap_ring_frames   = TAP_RING_FRAMES;
        L.tap_channels      = TAP_CHANNELS;
        L.tap_track_slots   = TAP_TRACK_SLOTS;
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
        const auto* in   = need(CLOCKWORK_ARENA_CLOCKWORK_IN_RING,      "arena has no ingress ring");
        const auto* out  = need(CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING,     "arena has no egress ring");
        const auto* nrt  = need(CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING, "arena has no NRT egress ring");
        const auto* ctl  = need(CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS,      "arena has no control block");
        const auto* met  = need(CLOCKWORK_ARENA_CLOCKWORK_METRICS,      "arena has no metrics");
        const auto* win  = need(CLOCKWORK_ARENA_GUEST_WINDOW, "arena has no guest window");
        const auto* clk  = need(CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE,  "arena has no clock state");
        const auto* scope = need(CLOCKWORK_ARENA_CLOCKWORK_SCOPE, "arena has no scope");
        const auto* taps = need(CLOCKWORK_ARENA_CLOCKWORK_TAPS,  "arena has no tap streams");
        const auto* ns   = need(CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS, "arena has no native stats");
        const auto* smp  = need(CLOCKWORK_ARENA_CLOCKWORK_SAMPLE_CLOCK, "arena has no sample clock");
        const auto* cm   = need(CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP,  "arena has no channel map");
        if (!in || !out || !nrt || !ctl || !met || !win || !clk || !scope || !taps || !ns || !smp || !cm)
            return false;
        L.blob_size           = h->arena_bytes;
        L.in_ring_offset      = in->offset;   L.in_ring_size      = in->bytes;
        L.out_ring_offset     = out->offset;  L.out_ring_size     = out->bytes;
        L.nrt_out_ring_offset = nrt->offset;  L.nrt_out_ring_size = nrt->bytes;
        // An arena without one is a reader's to accept: every verb then rides
        // the in ring, and the serve flag is never raised.
        if (const auto* cr = clockwork_arena_find(h, CLOCKWORK_ARENA_CLOCKWORK_CONTROL_RING)) {
            L.control_ring_offset = cr->offset; L.control_ring_size = cr->bytes;
        }
        L.control_offset      = ctl->offset;  L.control_bytes     = ctl->bytes;
        L.metrics_offset      = met->offset;  L.metrics_bytes     = met->geom[CLOCKWORK_GEOM_METRICS_FIELDS] * 4u;
        if (const auto* hist = clockwork_arena_find(h, CLOCKWORK_ARENA_CLOCKWORK_METRICS_HISTORY)) {
            L.metrics_history_offset = hist->offset; L.metrics_history_bytes = hist->bytes;
        }
        L.window_offset       = win->offset;  L.window_bytes      = win->bytes;
        L.clock_state_offset  = clk->offset;  L.clock_state_bytes = clk->bytes;
        L.audio_offset        = scope->offset;
        L.audio_slot_count    = scope->geom[CLOCKWORK_GEOM_SCOPE_SLOTS];
        L.audio_slot_bytes    = scope->geom[CLOCKWORK_GEOM_SCOPE_SLOT_BYTES];
        L.audio_header_bytes  = scope->geom[CLOCKWORK_GEOM_SCOPE_HEADER_BYTES];
        L.audio_frames        = scope->geom[CLOCKWORK_GEOM_SCOPE_FRAMES];
        L.audio_channels      = scope->geom[CLOCKWORK_GEOM_SCOPE_CHANNELS];
        L.audio_sample_rate   = scope->geom[CLOCKWORK_GEOM_SCOPE_SAMPLE_RATE];
        L.tap_offset        = taps->offset;
        L.tap_max           = taps->geom[CLOCKWORK_GEOM_TAP_SLOTS];
        L.tap_header_bytes  = taps->geom[CLOCKWORK_GEOM_TAP_HEADER_BYTES];
        L.tap_slot_bytes    = taps->geom[CLOCKWORK_GEOM_TAP_SLOT_BYTES];
        L.tap_slot_header   = taps->geom[CLOCKWORK_GEOM_TAP_SLOT_HEADER];
        L.tap_ring_frames   = taps->geom[CLOCKWORK_GEOM_TAP_RING_FRAMES];
        L.tap_channels      = taps->geom[CLOCKWORK_GEOM_TAP_CHANNELS];
        L.tap_track_slots   = taps->geom[CLOCKWORK_GEOM_TAP_TRACK_SLOTS];
        if (taps->geom[CLOCKWORK_GEOM_TAP_TRACK_FIRST] != L.tap_max) {
            if (why) *why = "track taps are not numbered after the guest's tap slots";
            return false;
        }
        L.native_stats_offset = ns->offset;   L.native_stats_bytes = ns->bytes;
        L.sample_clock_offset = smp->offset;  L.sample_clock_bytes = smp->bytes;
        L.channel_map_offset  = cm->offset;   L.channel_map_bytes  = cm->bytes;
        return L.check(why);
    }

    // Tap-stream slot `index` in ONE index space: the guest's slots first,
    // then the engine's track taps. nullptr past the end of both.
    uint8_t* tapSlotAt(uint8_t* base, uint32_t index) const {
        if (index < tap_max + tap_track_slots)
            return base + tap_offset + tap_header_bytes + size_t(index) * tap_slot_bytes;
        return nullptr;
    }
    uint32_t tapSlotCount() const { return tap_max + tap_track_slots; }

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
        if (control_ring_size != 0 && (control_ring_size <= CONTROL_RING_HEADER_SIZE
                                       || !inside(control_ring_offset, control_ring_size)))
            return fail("control ring too small or outside the blob");
        if (control_bytes < sizeof(RingCursors) || !inside(control_offset, control_bytes)) return fail("control block too small or outside the blob");
        if (metrics_bytes < sizeof(PerformanceMetrics) || !inside(metrics_offset, metrics_bytes)) return fail("metrics smaller than this reader's PerformanceMetrics, or outside the blob");
        if (!inside(window_offset, window_bytes)) return fail("window outside the blob");
        if (clock_state_bytes < sizeof(ClockworkClockState) || !inside(clock_state_offset, clock_state_bytes)) return fail("clock state too small or outside the blob");
        if (audio_header_bytes != SCOPE_HEADER_SIZE) return fail("scope slot shape differs from this reader's");
        if (audio_channels > SCOPE_CHANNELS) return fail("scope wider than this reader can interleave");
        if (audio_slot_count < SCOPE_SLOTS) return fail("arena has fewer scope slots than out and in");
        if (audio_slot_bytes < uint64_t(SCOPE_HEADER_SIZE) + uint64_t(audio_frames) * audio_channels * sizeof(float)) return fail("scope slot smaller than its declared ring");
        if (!inside(audio_offset, uint64_t(audio_slot_count) * audio_slot_bytes)) return fail("scope outside the blob");
        if (tap_slot_header != TAP_STREAM_HEADER_SIZE || tap_channels != TAP_STREAM_CHANNELS) return fail("tap slot shape differs from this reader's");
        if (tap_slot_bytes < uint64_t(TAP_STREAM_HEADER_SIZE) + uint64_t(tap_ring_frames) * tap_channels * sizeof(float)) return fail("tap slot smaller than its declared ring");
        if (!inside(tap_offset, uint64_t(tap_header_bytes) + (uint64_t(tap_max) + tap_track_slots) * tap_slot_bytes)) return fail("tap streams outside the blob");
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
    h->magic           = CLOCKWORK_ARENA_MAGIC;
    h->version         = CLOCKWORK_ARENA_VERSION;
    h->header_bytes    = ARENA_HEADER_SIZE;
    h->instance_id     = instance_id;
    h->generation      = 1;   // no instance yet, and no generation opened (docs/RESTARTS.md)
    h->arena_bytes     = ARENA_SIZE;
    h->clockwork_bytes = CLOCKWORK_HALF_SIZE;
    h->guest_offset    = GUEST_HALF_START;
    h->guest_bytes     = GUEST_HALF_SIZE;
    h->area_bytes      = sizeof(ClockworkArenaAreaEntry);
    h->entry_bytes     = sizeof(ClockworkArenaEntry);
    // The areas: the five in the arena by offset; the nine outside it with
    // no place yet (arenaSetArea, when the engine knows where they are).
    uint32_t na = 0;
    const auto area = [&](uint32_t id, uint32_t space, uint64_t at, uint64_t bytes) {
        ClockworkArenaAreaEntry& a = h->areas[na++];
        a.area = id; a.space = space;
        a.base_lo = uint32_t(at); a.base_hi = uint32_t(at >> 32);
        a.bytes_lo = uint32_t(bytes); a.bytes_hi = uint32_t(bytes >> 32);
    };
    area(CLOCKWORK_AREA_CLOCKWORK_STATE,     CLOCKWORK_SPACE_ARENA, CLOCKWORK_STATE_START,     CLOCKWORK_STATE_END - CLOCKWORK_STATE_START);
    area(CLOCKWORK_AREA_CLOCKWORK_AUDIO,     CLOCKWORK_SPACE_ARENA, CLOCKWORK_AUDIO_START,     CLOCKWORK_AUDIO_END - CLOCKWORK_AUDIO_START);
    area(CLOCKWORK_AREA_CLOCKWORK_TRANSPORT, CLOCKWORK_SPACE_ARENA, CLOCKWORK_TRANSPORT_START, CLOCKWORK_TRANSPORT_END - CLOCKWORK_TRANSPORT_START);
    area(CLOCKWORK_AREA_GUEST_PUBLIC,        CLOCKWORK_SPACE_ARENA, GUEST_PUBLIC_START,        GUEST_PUBLIC_END - GUEST_PUBLIC_START);
    area(CLOCKWORK_AREA_GUEST_PRIVATE,       CLOCKWORK_SPACE_ARENA, GUEST_PRIVATE_START,       GUEST_PRIVATE_END - GUEST_PRIVATE_START);
    area(CLOCKWORK_AREA_ASSETS,  CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_RETURNS, CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_GUEST_MEMORY_BULK, CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_GUEST_MEMORY_FAST, CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_STATIC_DATA,  CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_STACK,        CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_MALLOC,       CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_RUST_HEAP,    CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    area(CLOCKWORK_AREA_ENGINE_MEMORY, CLOCKWORK_SPACE_UNKNOWN, 0, 0);
    h->area_count = na;
    // The regions: id, where, the five facts, and the geometry a reader
    // walks it by.
    uint32_t n = 0;
    const auto put = [&](uint32_t id, uint32_t offset, uint32_t bytes, uint32_t owner, uint32_t audience,
                         uint32_t cadence, uint32_t tier, uint32_t lifetime,
                         std::initializer_list<uint32_t> geom = {}) {
        ClockworkArenaEntry& e = h->entries[n++];
        e.id = id; e.offset = offset; e.bytes = bytes; e.owner = owner; e.audience = audience;
        e.cadence = cadence; e.tier = tier; e.lifetime = lifetime;
        uint32_t i = 0;
        for (uint32_t g : geom) e.geom[i++] = g;
    };
    constexpr uint32_t kPublic    = CLOCKWORK_AUDIENCE_PUBLIC;
    constexpr uint32_t kTransport = CLOCKWORK_AUDIENCE_TRANSPORT;
    constexpr uint32_t kBlock = CLOCKWORK_CADENCE_BLOCK, kChange = CLOCKWORK_CADENCE_CHANGE, kBoot = CLOCKWORK_CADENCE_BOOT;
    constexpr uint32_t kFast = CLOCKWORK_TIER_FAST, kBulk = CLOCKWORK_TIER_BULK;
    constexpr uint32_t kColdBoot = CLOCKWORK_LIFETIME_COLD_BOOT, kRebuild = CLOCKWORK_LIFETIME_REBUILD,
                       kArena = CLOCKWORK_LIFETIME_ARENA;
    // clockwork-state
    // Cold boot, not rebuild: a count of what a teardown lost is still there
    // after it (docs/RESTARTS.md); the gauges are rewritten within a block.
    put(CLOCKWORK_ARENA_CLOCKWORK_METRICS,         METRICS_START,         METRICS_SIZE,         CLOCKWORK_OWNER_CLOCKWORK, kPublic,
        kBlock, kFast, kColdBoot, { METRICS_SIZE / 4u });
    // Cold boot only: a rebuild, or a new worklet over the same memory, carries the hour on.
    put(CLOCKWORK_ARENA_CLOCKWORK_METRICS_HISTORY, METRICS_HISTORY_START, METRICS_HISTORY_SIZE, CLOCKWORK_OWNER_CLOCKWORK, kPublic,
        kBlock, kFast, kColdBoot, { clockwork::metrics_history::kLevels, clockwork::metrics_history::kBuckets,
                                     clockwork::metrics_history::kMetrics, clockwork::metrics_history::kWords,
                                     clockwork::metrics_history::kHeaderBytes });
    put(CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS,    NATIVE_STATS_START,    NATIVE_STATS_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, kPublic,
        kBlock, kFast, kRebuild);
    put(CLOCKWORK_ARENA_CLOCKWORK_CLOCK_ANCHORS,   CLOCK_ANCHORS_START,   CLOCK_ANCHORS_SIZE,   CLOCKWORK_OWNER_HOST,      kPublic,
        kBlock, kFast, kColdBoot,
        { GLOBAL_OFFSET_START - CLOCK_ANCHORS_START });
    // The two the guest is handed read-only pointers to (DspConfig::clock, channel_map).
    put(CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE,     CLOCK_STATE_START,     CLOCK_STATE_SIZE,     CLOCKWORK_OWNER_CLOCKWORK,
        kPublic | CLOCKWORK_AUDIENCE_GUEST, kBlock, kFast, kColdBoot);
    put(CLOCKWORK_ARENA_CLOCKWORK_SAMPLE_CLOCK,    SAMPLE_CLOCK_START,    SAMPLE_CLOCK_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, kPublic,
        kBlock, kFast, kRebuild);
    // Cold boot, not rebuild: the streams bound over channels outlive the
    // guest instance (docs/RESTARTS.md, "port routing"); a device change
    // rewrites the device's own words, as any attach rewrites an entry.
    put(CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP,     CHANNEL_MAP_START,     CHANNEL_MAP_SIZE,     CLOCKWORK_OWNER_CLOCKWORK,
        kPublic | CLOCKWORK_AUDIENCE_GUEST, kChange, kFast, kColdBoot);
    // clockwork-audio
    put(CLOCKWORK_ARENA_CLOCKWORK_SCOPE,           SCOPE_START,           SCOPE_SIZE,           CLOCKWORK_OWNER_CLOCKWORK, kPublic,
        kBlock, kFast, kRebuild,
        { SCOPE_SLOTS, SCOPE_SLOT_SIZE, SCOPE_HEADER_SIZE, SCOPE_FRAMES, SCOPE_CHANNELS, SCOPE_SAMPLE_RATE });
    put(CLOCKWORK_ARENA_CLOCKWORK_TAPS,            TAPS_START,            TAPS_SIZE,            CLOCKWORK_OWNER_CLOCKWORK, kPublic,
        kBlock, kBulk, kRebuild,
        { TAPS_SLOTS, TAPS_HEADER_SIZE, TAP_SLOT_SIZE, TAP_SLOT_HEADER_SIZE, TAP_RING_FRAMES, TAP_CHANNELS,
          TAP_TRACK_SLOTS, TAP_TRACK_SLOT_BASE });
    // clockwork-transport
    put(CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS,    RING_CURSORS_START,    RING_CURSORS_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, kTransport,
        kBlock, kFast, kRebuild);
    put(CLOCKWORK_ARENA_CLOCKWORK_NODE_ID_COUNTER, NODE_ID_COUNTER_START, NODE_ID_COUNTER_SIZE, CLOCKWORK_OWNER_CLIENT,    kTransport,
        kChange, kFast, kColdBoot);
    // Cold boot, not rebuild: messages queued for a torn-down instance stay in
    // the ring and the next drain refuses them, below the teardown's mark
    // (docs/RESTARTS.md).
    put(CLOCKWORK_ARENA_CLOCKWORK_IN_RING,         IN_RING_START,         IN_RING_SIZE,         CLOCKWORK_OWNER_CLIENT,    kTransport,
        kBlock, kFast, kColdBoot,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)) });
    // Cold boot, not rebuild: what an instance said stays for its readers
    // across the rebuild, and the next instance's setup follows it
    // (docs/RESTARTS.md). Their cursors live in the ring cursors' region and
    // are reset with them.
    put(CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING,     RT_OUT_RING_START,     RT_OUT_RING_SIZE,     CLOCKWORK_OWNER_CLOCKWORK, kTransport,
        kBlock, kFast, kColdBoot,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)) });
    put(CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING,    NRT_OUT_RING_START,    NRT_OUT_RING_SIZE,    CLOCKWORK_OWNER_CLOCKWORK, kTransport,
        kChange, kFast, kColdBoot,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)) });
    // Clockwork's own control verbs (ControlRingCursors above): lifetime
    // arena, so a rebuild neither resets it nor has anything to carry. The
    // sixth geometry word is where the ring starts within the region.
    put(CLOCKWORK_ARENA_CLOCKWORK_CONTROL_RING,    CONTROL_RING_START,    CONTROL_RING_SIZE,    CLOCKWORK_OWNER_CLIENT,    kTransport,
        kChange, kFast, kArena,
        { MAX_MESSAGE_SIZE, MESSAGE_MAGIC, PADDING_MAGIC, RING_PADDING_MARKER, static_cast<uint32_t>(sizeof(Message)),
          CONTROL_RING_HEADER_SIZE });
    // guest-public. The window's identity (magic, version) is the guest's to
    // declare and is written when it binds — arenaSetGuestWindowIdentity — so
    // it is 0 here.
    put(CLOCKWORK_ARENA_GUEST_WINDOW,              GUEST_WINDOW_START,    GUEST_WINDOW_SIZE,    CLOCKWORK_OWNER_GUEST,     kPublic,
        kChange, kBulk, kRebuild);
    // guest-private
    put(CLOCKWORK_ARENA_GUEST_CONFIG,              GUEST_CONFIG_START,    GUEST_CONFIG_SIZE,    CLOCKWORK_OWNER_HOST,
        CLOCKWORK_AUDIENCE_GUEST | CLOCKWORK_AUDIENCE_HOST, kBoot, kBulk, kColdBoot);
    put(CLOCKWORK_ARENA_GUEST_PERSIST,             GUEST_PERSIST_START,   GUEST_PERSIST_SIZE,   CLOCKWORK_OWNER_GUEST,
        CLOCKWORK_AUDIENCE_GUEST, kChange, kBulk, kColdBoot);
    h->entry_count = n;
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<uint32_t>*>(&h->state)->store(CLOCKWORK_ARENA_PUBLISHED,
                                                              std::memory_order_release);
}

// Where one of the three areas outside the arena is (assets, returns, the
// heap), once the engine knows: the space its base is measured in, the base,
// the size. Written after the table is published, each word a plain store
// and the space last with a release, so a reader that took the table before
// sees UNKNOWN, which means "not placed yet", and asks again. A build or a
// host with no such area leaves it UNKNOWN for good.
inline void arenaSetArea(uint8_t* base, uint32_t area, uint32_t space, uint64_t at, uint64_t bytes) {
    auto* h = reinterpret_cast<ClockworkArenaHeader*>(base);
    auto* a = const_cast<ClockworkArenaAreaEntry*>(clockwork_arena_area(h, area));
    if (!a) return;
    reinterpret_cast<std::atomic<uint32_t>*>(&a->bytes_lo)->store(uint32_t(bytes), std::memory_order_relaxed);
    reinterpret_cast<std::atomic<uint32_t>*>(&a->bytes_hi)->store(uint32_t(bytes >> 32), std::memory_order_relaxed);
    reinterpret_cast<std::atomic<uint32_t>*>(&a->base_lo)->store(uint32_t(at), std::memory_order_relaxed);
    reinterpret_cast<std::atomic<uint32_t>*>(&a->base_hi)->store(uint32_t(at >> 32), std::memory_order_relaxed);
    reinterpret_cast<std::atomic<uint32_t>*>(&a->space)->store(space, std::memory_order_release);
}

// The header at the front of an arena.
inline const ClockworkArenaHeader* arenaHeader(const void* base) {
    return reinterpret_cast<const ClockworkArenaHeader*>(base);
}

// The generation word (ClockworkArenaHeader::generation), as the engine moves
// it and as a reader in this process loads it. Odd while there is no guest
// instance, even while there is one; the rest is the generation the last build
// opened (docs/RESTARTS.md). Two moves, each one step: the instance going
// (even -> odd) at the start of a teardown, before anything else of it, and
// the instance up (odd -> even) once dsp_new has succeeded. A move that is
// not due — a teardown with no instance up — is not a move.
inline std::atomic<uint32_t>& arenaGenerationWord(void* base) {
    return *reinterpret_cast<std::atomic<uint32_t>*>(
        &reinterpret_cast<ClockworkArenaHeader*>(base)->generation);
}
inline uint32_t arenaGenerationLoad(const void* base) {
    return reinterpret_cast<const std::atomic<uint32_t>*>(
        &arenaHeader(base)->generation)->load(std::memory_order_acquire);
}
inline uint32_t arenaInstanceGeneration(const void* base) { return arenaGenerationLoad(base) >> 1u; }
inline bool     arenaHasInstance(const void* base)        { return (arenaGenerationLoad(base) & 1u) == 0u; }
inline void arenaInstanceDown(void* base) {
    auto& w = arenaGenerationWord(base);
    const uint32_t v = w.load(std::memory_order_acquire);
    if ((v & 1u) == 0u) w.store(v + 1u, std::memory_order_release);
}
inline void arenaInstanceUp(void* base) {
    auto& w = arenaGenerationWord(base);
    const uint32_t v = w.load(std::memory_order_acquire);
    if ((v & 1u) != 0u) w.store(v + 1u, std::memory_order_release);
}

// What the guest says its window is (DspInfo::window_magic, window_version),
// into the window entry's geometry, when the guest binds. The one thing in
// the table written after it is published — so a client knows which guest,
// and which layout, before it casts a pointer into the window. Two plain
// word stores: a reader that took the table before the guest bound sees
// zeros, which mean "not yet declared", and asks again.
inline void arenaSetGuestWindowIdentity(uint8_t* base, uint32_t magic, uint32_t version) {
    auto* h = reinterpret_cast<ClockworkArenaHeader*>(base);
    auto* e = const_cast<ClockworkArenaEntry*>(clockwork_arena_find(h, CLOCKWORK_ARENA_GUEST_WINDOW));
    if (!e) return;
    reinterpret_cast<std::atomic<uint32_t>*>(&e->geom[CLOCKWORK_GEOM_WINDOW_VERSION])
        ->store(version, std::memory_order_relaxed);
    reinterpret_cast<std::atomic<uint32_t>*>(&e->geom[CLOCKWORK_GEOM_WINDOW_MAGIC])
        ->store(magic, std::memory_order_release);
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
CLOCKWORK_ASSERT_OFFSET(ClockworkClockState, generation,        36,
                 "js/lib/clockwork_clock_protocol.js SC_GENERATION_I32");

// RingCursors ↔ js/lib/ring_cursor_offsets.js (JS exports byte offsets directly)
CLOCKWORK_ASSERT_OFFSET(RingCursors, in_head,        0,  "js/lib/ring_cursor_offsets.js IN_HEAD");
CLOCKWORK_ASSERT_OFFSET(RingCursors, in_tail,        4,  "js/lib/ring_cursor_offsets.js IN_TAIL");
CLOCKWORK_ASSERT_OFFSET(RingCursors, out_head,       8,  "js/lib/ring_cursor_offsets.js OUT_HEAD");
CLOCKWORK_ASSERT_OFFSET(RingCursors, out_tail,       12, "js/lib/ring_cursor_offsets.js OUT_TAIL");
CLOCKWORK_ASSERT_OFFSET(RingCursors, nrt_out_head,     16, "js/lib/ring_cursor_offsets.js NRT_OUT_HEAD");
CLOCKWORK_ASSERT_OFFSET(RingCursors, nrt_out_tail,     20, "js/lib/ring_cursor_offsets.js NRT_OUT_TAIL");
CLOCKWORK_ASSERT_OFFSET(RingCursors, in_sequence,    24, "js/lib/ring_cursor_offsets.js IN_SEQUENCE");
CLOCKWORK_ASSERT_OFFSET(RingCursors, out_sequence,   28, "js/lib/ring_cursor_offsets.js OUT_SEQUENCE");
CLOCKWORK_ASSERT_OFFSET(RingCursors, nrt_out_sequence, 32, "js/lib/ring_cursor_offsets.js NRT_OUT_SEQUENCE");
CLOCKWORK_ASSERT_OFFSET(RingCursors, status_flags,   36, "js/lib/ring_cursor_offsets.js STATUS_FLAGS");
CLOCKWORK_ASSERT_OFFSET(RingCursors, in_write_lock,  40, "js/lib/ring_cursor_offsets.js IN_WRITE_LOCK");
CLOCKWORK_ASSERT_OFFSET(RingCursors, out_read,       44, "js/lib/ring_cursor_offsets.js OUT_READ");
CLOCKWORK_ASSERT_OFFSET(RingCursors, nrt_out_read,   48, "js/lib/ring_cursor_offsets.js NRT_OUT_READ");
CLOCKWORK_ASSERT_OFFSET(RingCursors, _padding,       52, "js/lib/ring_cursor_offsets.js padding");

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
CLOCKWORK_ASSERT_METRIC(clockwork_commit,               39);
CLOCKWORK_ASSERT_METRIC(scheduler_capacity,             40);
CLOCKWORK_ASSERT_METRIC(scheduler_pool_bytes,           41);
CLOCKWORK_ASSERT_METRIC(audio_sample_rate,               42);
CLOCKWORK_ASSERT_METRIC(audio_block_size,                43);
CLOCKWORK_ASSERT_METRIC(audio_output_channels,           44);
CLOCKWORK_ASSERT_METRIC(audio_input_channels,            45);
CLOCKWORK_ASSERT_METRIC(clock_tempo_mbpm,                46);
CLOCKWORK_ASSERT_METRIC(clock_beat_centi,                47);
CLOCKWORK_ASSERT_METRIC(clock_phase_centi,               48);
CLOCKWORK_ASSERT_METRIC(clock_playing,                   49);
CLOCKWORK_ASSERT_METRIC(guest_pool_used_bytes,           53);
CLOCKWORK_ASSERT_METRIC(guest_pool_peak_bytes,           54);
CLOCKWORK_ASSERT_METRIC(egress_frames_refused,           55);
CLOCKWORK_ASSERT_METRIC(rust_heap_used_bytes,            56);
CLOCKWORK_ASSERT_METRIC(rust_heap_fallbacks,             57);
CLOCKWORK_ASSERT_METRIC(guest_malloc_bytes,              58);
CLOCKWORK_ASSERT_METRIC(guest_mallocs_processing,        59);
CLOCKWORK_ASSERT_METRIC(mallocs_after_boot,              60);
CLOCKWORK_ASSERT_METRIC(malloc_bytes_after_boot,         61);
CLOCKWORK_ASSERT_METRIC(egress_frames_undelivered,       62);
CLOCKWORK_ASSERT_METRIC(clock_drift_us,                  50);
CLOCKWORK_ASSERT_METRIC(clock_anchors,                   63);
CLOCKWORK_ASSERT_METRIC(scheduler_late_dropped,          64);
CLOCKWORK_ASSERT_METRIC(cues_dropped,                    65);

// METRICS_SIZE must cover the whole struct and stay a multiple of 8: the arena
// regions that follow (ClockworkClockState) are 8-byte aligned and read via
// Float64/BigInt64 views; these asserts make an odd field count a build error.
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
