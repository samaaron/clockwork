// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/*
 * lanes.cpp — the engine boundary, implemented over the engine's existing
 * transport state. See lanes.h for the contract.
 *
 * Ingress and the NRT producer delegate to RingBufferWriter; the egress
 * drains delegate to clockwork_drain_ring (ring_drain.h, also used by the native
 * RingReader thread); the tick is process_audio.
 */
#include "lanes.h"
#include "lanes_internal.h"
#include "ring_drain.h"

#include <atomic>
#include <cstring>

#include "../audio_processor.h"          // arena globals + process_audio + accessors
#include "../audio_config.h"             // block-size and channel caps
#include "../shared_memory.h"            // layout, ControlPointers, EgressRoute
#include "../workers/RingBufferWriter.h" // the multi-producer ring writer

// ── internal state ──────────────────────────────────────────────────────────

// NRT egress producer lock. Producers on control threads serialise their
// [route][osc] framing through here; this is the process-wide lock for the
// NRT-out lane (the engine's OscEgress delegates to clockwork_egress_nrt_write).
static std::atomic<int32_t> g_nrt_egress_lock{0};

// Largest NRT egress frame the producer accepts. Sized for the biggest
// producer — the device reports, which reach several KB on device-heavy
// Windows machines — but never more than half the ring so a frame can
// always drain (small-profile builds have a 4 KB NRT-out ring).
static constexpr uint32_t kNrtEgressMax =
    NRT_OUT_BUFFER_SIZE / 2 < 8192 ? NRT_OUT_BUFFER_SIZE / 2 : 8192;

// Per-lane consumer state (single consumer per lane, by contract). Process
// lifetime; init_memory() resets it alongside the ring sequence counters via
// clockwork_lanes_reset_drains.
static ClockworkDrainState g_rt_drain_state;
static ClockworkDrainState g_nrt_drain_state;

extern "C" {

void clockwork_lanes_reset_drains(void) {
    g_rt_drain_state.lastSeq  = -1;
    g_nrt_drain_state.lastSeq = -1;
}

// Fresh ring epoch. The RT-out ring's single producer (the audio thread) is
// stopped by the caller, but IN and NRT-out producers run through a cold-swap
// rebuild: transport ingress threads, and clockwork_log / clockwork_egress_nrt_write from
// any engine worker. Each serialises its whole read-head→publish sequence on
// its ring's writer spinlock (RingBufferWriter::write), so owning that lock
// across the reset linearises it — an in-flight write lands wholly in the old
// epoch (discarded here) or wholly in the fresh one, never interleaved with
// the zeroing. The lock words are never zeroed: releasing the spinlock is
// what returns them to 0, and a blind store would unlock under a holder.
void clockwork_lanes_reset_rings(void) {
    if (!shared_memory || !control) return;

    const auto spin_acquire = [](std::atomic<int32_t>& lock) {
        int32_t expected = 0;
        while (!lock.compare_exchange_weak(expected, 1,
                std::memory_order_acquire, std::memory_order_relaxed))
            expected = 0;
    };

    spin_acquire(control->in_write_lock);
    control->in_head.store(0, std::memory_order_relaxed);
    control->in_tail.store(0, std::memory_order_relaxed);
    control->in_sequence.store(0, std::memory_order_relaxed);
    control->in_write_lock.store(0, std::memory_order_release);

    spin_acquire(g_nrt_egress_lock);
    control->nrt_out_head.store(0, std::memory_order_relaxed);
    control->nrt_out_tail.store(0, std::memory_order_relaxed);
    control->nrt_out_sequence.store(0, std::memory_order_relaxed);
    g_nrt_egress_lock.store(0, std::memory_order_release);

    control->out_head.store(0, std::memory_order_relaxed);
    control->out_tail.store(0, std::memory_order_relaxed);
    control->out_sequence.store(0, std::memory_order_relaxed);
}

// ── Ingress ─────────────────────────────────────────────────────────────────

bool clockwork_ingress_write(const uint8_t* osc, uint32_t len, uint32_t source_id) {
    if (!memory_initialized || !shared_memory || !control || !osc || len == 0)
        return false;
    return RingBufferWriter::write(
        shared_memory + IN_BUFFER_START, IN_BUFFER_SIZE,
        &control->in_head, &control->in_tail,
        &control->in_sequence, &control->in_write_lock,
        osc, len, source_id);
}

// ── Egress ──────────────────────────────────────────────────────────────────

// Both egress rings carry Message frames whose payload is [route:u32][osc];
// the drains peel the route word before the callback sees the OSC bytes.
static uint32_t drain_egress_ring(uint8_t* buffer, uint32_t size,
                                  std::atomic<int32_t>* head,
                                  std::atomic<int32_t>* tail,
                                  ClockworkDrainState& st,
                                  const ClockworkDrainMetrics& m,
                                  ClockworkEgressFn fn, void* ctx,
                                  uint32_t max_frames) {
    return clockwork_drain_ring(buffer, size, head, tail, st, m, max_frames,
        [fn, ctx](uint32_t sourceId, const uint8_t* payload, uint32_t n, uint32_t seq) {
            if (n >= EGRESS_ROUTE_SIZE) {
                uint32_t route;
                std::memcpy(&route, payload, sizeof(route));
                fn(ctx, sourceId, route,
                   payload + EGRESS_ROUTE_SIZE, n - EGRESS_ROUTE_SIZE, seq);
            }
            return ClockworkDrainVerdict::Consume;
        });
}

uint32_t clockwork_egress_rt_drain(ClockworkEgressFn fn, void* ctx, uint32_t max_frames) {
    if (!memory_initialized || !shared_memory || !control || !fn) return 0;
    // RT egress traffic counts into the segment-resident metrics so external
    // observers see reply/notification volume and ring health.
    ClockworkDrainMetrics m;
    if (metrics) {
        m.received  = &metrics->osc_in_messages_received;
        m.bytes     = &metrics->osc_in_bytes_received;
        m.corrupted = &metrics->osc_in_corrupted;
        m.seqGaps   = &metrics->messages_sequence_gaps;
    }
    return drain_egress_ring(shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                             &control->out_head, &control->out_tail,
                             g_rt_drain_state, m, fn, ctx, max_frames);
}

uint32_t clockwork_egress_nrt_drain(ClockworkEgressFn fn, void* ctx, uint32_t max_frames) {
    if (!memory_initialized || !shared_memory || !control || !fn) return 0;
    // The same reply counters as the RT ring: which ring a reply rode is an
    // implementation detail (a guest's off-thread stage answers here, its
    // audio-thread stage there), and a client's "replies received" is the
    // sum. Sequence gaps stay per ring — this ring numbers its own frames.
    ClockworkDrainMetrics m;
    if (metrics) {
        m.received  = &metrics->osc_in_messages_received;
        m.bytes     = &metrics->osc_in_bytes_received;
        m.corrupted = &metrics->osc_in_corrupted;
    }
    return drain_egress_ring(shared_memory + NRT_OUT_BUFFER_START, NRT_OUT_BUFFER_SIZE,
                             &control->nrt_out_head, &control->nrt_out_tail,
                             g_nrt_drain_state, m, fn, ctx, max_frames);
}

bool clockwork_egress_nrt_write(uint32_t route, uint32_t token,
                         const uint8_t* osc, uint32_t len) {
    if (!memory_initialized || !shared_memory || !control || !osc || len == 0)
        return false;
    // Subtraction form: len + EGRESS_ROUTE_SIZE would wrap for len near
    // UINT32_MAX and slip past the bound into the stack memcpy below.
    if (len > kNrtEgressMax - EGRESS_ROUTE_SIZE) return false;

    uint8_t buf[kNrtEgressMax];
    std::memcpy(buf, &route, sizeof(route));
    std::memcpy(buf + sizeof(route), osc, len);
    return RingBufferWriter::write(
        shared_memory + NRT_OUT_BUFFER_START, NRT_OUT_BUFFER_SIZE,
        &control->nrt_out_head, &control->nrt_out_tail,
        &control->nrt_out_sequence, &g_nrt_egress_lock,
        buf, len + EGRESS_ROUTE_SIZE, token);
}

// ── Tick ────────────────────────────────────────────────────────────────────

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
bool clockwork_tick(double ntp_now, uint32_t out_channels, uint32_t in_channels) {
    return process_audio(ntp_now, out_channels, in_channels);
}

const float* clockwork_audio_out(void) {
    return reinterpret_cast<const float*>(get_audio_output_bus());
}

float* clockwork_audio_in(void) {
    return reinterpret_cast<float*>(get_audio_input_bus());
}

uint32_t clockwork_block_size(void) {
    return static_cast<uint32_t>(get_audio_buffer_samples());
}

double clockwork_sample_rate(void) {
    return get_audio_sample_rate();
}

int64_t clockwork_block_time(void) {
    return get_audio_block_time();
}

// ── Init ──────────────────────────────────────────────────────────────────────

// The arena override init_memory() consults (set by the native backend when a
// public POSIX segment exists; null otherwise). Declared here so clockwork_init
// copies the guest's config block into the SAME arena init_memory will read:
// reading from ring_buffer_storage while the host wrote to a segment would see
// zeros.
extern uint8_t* g_external_segment;

// How many hosts hold the engine. Not atomic: attach and detach are control
// calls, made from a host's setup path, never from the audio thread — the same
// place clockwork_init has always been called from.
static uint32_t g_attached = 0;

// Can the running engine serve a host that asked for this?
//
// Rate and block are exact: every host in the process renders the same blocks
// at the same rate, so they are not negotiable per-caller. A block of 0 means
// "whatever is running". Channels only have to be ENOUGH — a joiner that needs
// two of an engine that opened eight is served.
static bool geometry_serves(double sample_rate, uint32_t block_size,
                            uint32_t input_channels, uint32_t output_channels) {
    if (sample_rate != g_clockwork_config.sample_rate)              return false;
    if (block_size != 0 && block_size != g_clockwork_config.block_size) return false;
    if (input_channels  > g_clockwork_config.input_channels)        return false;
    if (output_channels > g_clockwork_config.output_channels)       return false;
    return true;
}

int clockwork_attach(double   sample_rate,
                uint32_t block_size,
                uint32_t input_channels,
                uint32_t output_channels,
                uint32_t verbosity,
                void*    guest_arena,
                uint32_t guest_arena_bytes,
                const void* guest_config,
                uint32_t    guest_config_bytes,
                uint32_t    arena_bytes,
                const void* inbox,
                uint32_t    inbox_bytes,
                void*       outbox,
                uint32_t    outbox_bytes) {
    if (g_attached == 0) {
        clockwork_init(sample_rate, block_size, input_channels, output_channels,
                  verbosity, guest_arena, guest_arena_bytes,
                  guest_config, guest_config_bytes, arena_bytes,
                  inbox, inbox_bytes, outbox, outbox_bytes);
        g_attached = 1;
        return CLOCKWORK_ATTACH_BOOTED;
    }

    if (!geometry_serves(sample_rate, block_size, input_channels, output_channels)) {
        clockwork_log("[attach] refused: running at %.0f Hz, %u frames, %u in / %u out; "
                "asked for %.0f Hz, %u frames, %u in / %u out",
                g_clockwork_config.sample_rate, g_clockwork_config.block_size,
                g_clockwork_config.input_channels, g_clockwork_config.output_channels,
                sample_rate, block_size, input_channels, output_channels);
        return CLOCKWORK_ATTACH_REFUSED;
    }

    // Joined. Deliberately nothing else: the arguments are not applied, the
    // guest config is not copied over the running one, and the arena is not
    // re-pointed. A joiner takes what is there.
    ++g_attached;
    return CLOCKWORK_ATTACH_JOINED;
}

void     clockwork_detach(void)   { if (g_attached) --g_attached; }
uint32_t clockwork_attached(void) { return g_attached; }

void clockwork_init(double   sample_rate,
              uint32_t block_size,
              uint32_t input_channels,
              uint32_t output_channels,
              uint32_t verbosity,
              void*    guest_arena,
              uint32_t guest_arena_bytes,
              const void* guest_config,
              uint32_t    guest_config_bytes,
              uint32_t    arena_bytes,
              const void* inbox,
              uint32_t    inbox_bytes,
              void*       outbox,
              uint32_t    outbox_bytes) {
    // THE SOLE-HOST CALL. Reconfiguring is routine — a device switch arrives
    // here — but once a second host has attached there is no longer any such
    // thing as "the" host's geometry, and applying one host's would silently
    // reconfigure the other's engine. Refused rather than obeyed. See
    // clockwork_attach in lanes.h.
    if (g_attached > 1) {
        clockwork_log("[init] refused: %u hosts are attached, so there is no "
                "single host whose geometry to apply. Use clockwork_attach.",
                g_attached);
        return;
    }
    // However this host arrived, it is now the one attached.
    if (g_attached == 0) g_attached = 1;

    // Copy the guest's bytes into the region reserved for them, without
    // looking at any of them. NULL means the host has already put them there
    // (the web one has no C pointer to pass) or that there are none.
    if (guest_config && guest_config_bytes) {
        uint8_t* arena = g_external_segment ? g_external_segment : ring_buffer_storage;
        const uint32_t n = guest_config_bytes < GUEST_CONFIG_SIZE
                         ? guest_config_bytes : GUEST_CONFIG_SIZE;
        // Refuse the tail rather than the block: a config that does not fit is
        // a config the host and the region disagree about, and truncating it
        // silently boots a guest that cannot tell it was cut short. The web
        // side throws on the same condition (writeGuestConfigToMemory).
        if (guest_config_bytes > GUEST_CONFIG_SIZE) {
            clockwork_log("[init] guest config is %u bytes but the region holds %u "
                    "— raise GUEST_CONFIG_SIZE; booting with a truncated block",
                    guest_config_bytes, (uint32_t)GUEST_CONFIG_SIZE);
        }
        std::memcpy(arena + GUEST_CONFIG_START, guest_config, n);
    }

    clockwork_set_engine_config(sample_rate, block_size, input_channels,
                           output_channels, verbosity,
                           guest_arena, guest_arena_bytes, arena_bytes,
                           inbox, inbox_bytes, outbox, outbox_bytes);
    init_memory();
}

void clockwork_set_channel_ceilings(uint32_t input_channels, uint32_t output_channels) {
    g_clockwork_config.input_channels  = input_channels;
    g_clockwork_config.output_channels = output_channels;
}

void clockwork_declare_fp_env(uint32_t env) {
    g_clockwork_config.fp_env = env;
}

// ── Layout ──────────────────────────────────────────────────────────────────

const ClockworkArenaHeader* clockwork_arena_header(void) {
    return arenaHeader(get_shared_memory_base());
}

void* clockwork_lanes_base(void) {
    return get_shared_memory_base();
}

}  // extern "C"
