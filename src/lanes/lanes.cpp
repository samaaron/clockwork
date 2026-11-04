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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "../audio_processor.h"          // init_memory, process_audio
#include "../clockwork_config.h"         // clockwork_rust_log_install
#include "../audio_config.h"             // block-size and channel caps
#include "../clockwork_prefix.h"         // clockwork_sys_claims: whose message this is
#include "../clockwork_sys.h"            // clockwork_sys_refuse, the refusal reasons
#include "../shared_memory.h"            // layout, RingCursors, EgressRoute
#include "../in_ring_door.h"              // the in ring's boundary
#include "../workers/RingBufferWriter.h" // the multi-producer ring writer
#include "../workers/wait_word.h"        // what a client waits on for egress
#include "../core_state.h"               // a core's Egress

// ── internal state ──────────────────────────────────────────────────────────

// Largest NRT egress frame the producer accepts for a core laid out as `L`.
// Sized for the biggest producer — the device reports, which reach several
// KB on device-heavy Windows machines — but never more than half the ring so
// a frame can always drain (small profiles have a 4 KB NRT-out ring).
static uint32_t nrt_egress_max(const clockwork::Layout& L) {
    return std::min(L.profile.nrt_out_ring_bytes / 2, 8192u);
}

extern "C" {

// Fresh ring epochs. The IN and NRT-out producers run through a cold-swap
// rebuild: transport ingress threads, and clockwork_log / clockwork_egress_nrt_write
// from any engine worker. Each serialises its whole read-head→publish
// sequence on its ring's writer spinlock (RingBufferWriter::write), so owning
// that lock across the reset linearises it — an in-flight write lands wholly
// in the old epoch (discarded here) or wholly in the fresh one, never
// interleaved with the zeroing. The lock words are never zeroed: releasing
// the spinlock is what returns them to 0, and a blind store would unlock
// under a holder.
static void spin_acquire(std::atomic<int32_t>& lock) { RingBufferWriter::lock(&lock); }

void clockwork_lanes_reset_in_ring(ClockworkCore* core) {
    clockwork::Ingress* in = clockwork::ingress(core);
    RingCursors* const cursors = clockwork::cursors_in(clockwork::arena_base(core), clockwork::layout_of(core));
    if (!in || !cursors) return;
    spin_acquire(cursors->in_write_lock);
    cursors->in_head.store(0, std::memory_order_relaxed);
    cursors->in_tail.store(0, std::memory_order_relaxed);
    cursors->in_sequence.store(0, std::memory_order_relaxed);
    cursors->in_write_lock.store(0, std::memory_order_release);
    in->drain.lastSeq = -1;
    in->marks.clear();
}

// The RT-out ring's one writer is stopped by the caller: a new arena has no
// audio thread yet.
void clockwork_lanes_reset_rt_out_ring(ClockworkCore* core) {
    clockwork::Egress* e = clockwork::egress(core);
    RingCursors* const cursors = clockwork::cursors_in(clockwork::arena_base(core), clockwork::layout_of(core));
    if (!e || !cursors) return;
    cursors->out_head.store(0, std::memory_order_relaxed);
    cursors->out_tail.store(0, std::memory_order_relaxed);
    cursors->out_read.store(0, std::memory_order_relaxed);
    cursors->out_sequence.store(0, std::memory_order_relaxed);
    e->rt_drain.lastSeq = -1;
    e->rt_out_said = 0;
}

void clockwork_lanes_reset_nrt_out_ring(ClockworkCore* core) {
    clockwork::Egress* e = clockwork::egress(core);
    RingCursors* const cursors = clockwork::cursors_in(clockwork::arena_base(core), clockwork::layout_of(core));
    if (!e || !cursors) return;
    spin_acquire(e->nrt_write_lock);
    cursors->nrt_out_head.store(0, std::memory_order_relaxed);
    cursors->nrt_out_tail.store(0, std::memory_order_relaxed);
    cursors->nrt_out_read.store(0, std::memory_order_relaxed);
    cursors->nrt_out_sequence.store(0, std::memory_order_relaxed);
    e->nrt_write_lock.store(0, std::memory_order_release);
    e->nrt_drain.lastSeq = -1;
}

// ── Ingress ─────────────────────────────────────────────────────────────────

bool clockwork_ingress_write(ClockworkCore* core, const uint8_t* osc, uint32_t len, uint32_t source_id) {
    return clockwork_ingress_write_status(core, osc, len, source_id) == InRingWrite::Written;
}

} // extern "C"

InRingWrite clockwork_ingress_write_status(ClockworkCore* core, const uint8_t* osc, uint32_t len, uint32_t source_id) {
    uint8_t* const base = clockwork::laid_out_base(core);
    if (!clockwork::ingress(core) || !base || !osc || len == 0)
        return InRingWrite::Full;
    RingCursors* const cursors = clockwork::cursors_in(base, clockwork::layout_of(core));
    // A control verb of clockwork's, while the engine serves the control ring
    // (lanes.h, clockwork_control_serve): written there, where no audio
    // block is between it and its answer. The same rule clockwork_client_send
    // reads from the same bytes, so a client in any process does the same.
    const uint32_t flags = cursors->status_flags.load(std::memory_order_acquire);
    if ((flags & STATUS_CONTROL_SERVED) && clockwork_sys_control_takes(osc, len)) {
        const uint32_t control = clockwork::layout_of(core).control_ring_start;
        auto* ctl = reinterpret_cast<ControlRingCursors*>(base + control);
        if (!RingBufferWriter::write(
                base + control + CONTROL_RING_HEADER_SIZE,
                CONTROL_RING_SIZE - CONTROL_RING_HEADER_SIZE,
                &ctl->head, &ctl->tail, &ctl->sequence, &ctl->write_lock,
                osc, len, source_id))
            return InRingWrite::Full;
        clockwork_control_wake(core);
        return InRingWrite::Written;
    }
    // No guest instance to take it: refused at the boundary (in_ring_door.h),
    // with the word on the NRT egress for the source that sent it, as the
    // client ABI's status says the same to its caller (docs/RESTARTS.md).
    // Clockwork's own verbs go through and wait in the ring for the engine,
    // instance or none.
    const clockwork::Layout& L = clockwork::layout_of(core);
    const InRingWrite wrote = in_ring_write(base, base + L.in_ring_start, L.profile.in_ring_bytes,
                                            cursors, osc, len, source_id);
    if (wrote == InRingWrite::NoInstance)
        clockwork_sys_refuse(osc, len, CLOCKWORK_REFUSED_NO_INSTANCE,
            arenaInstanceGeneration(base),
            [core, source_id](const uint8_t* d, uint32_t n) {
                clockwork_egress_nrt_write(core, EGRESS_REPLY, source_id, d, n);
            });
    return wrote;
}

bool clockwork_egress_nrt_write_parts(ClockworkCore* core, uint32_t route, uint32_t token,
                                      const RingPart* parts, uint32_t n_parts) {
    static constexpr uint32_t kMaxParts = 8;
    clockwork::Egress* e = clockwork::egress(core);
    uint8_t* const base = clockwork::laid_out_base(core);
    if (!e || !base || n_parts == 0 || n_parts > kMaxParts - 1)
        return false;
    RingCursors* const cursors = clockwork::cursors_in(base, clockwork::layout_of(core));
    PerformanceMetrics* const metrics = clockwork::metrics_in(base);
    // More than a frame carries is refused and counted, as a full ring's
    // refusal is: the frame reaches no one either way. Summed so that no
    // part near UINT32_MAX can wrap past the bound.
    const clockwork::Layout& L = clockwork::layout_of(core);
    const uint32_t most = nrt_egress_max(L);
    uint32_t len = 0;
    for (uint32_t i = 0; i < n_parts; ++i) {
        if (parts[i].size > most - EGRESS_ROUTE_SIZE - len) {
            metrics->egress_frames_refused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        len += parts[i].size;
    }
    if (len == 0) return false;
    std::array<RingPart, kMaxParts> frame{};
    frame[0] = RingPart{ &route, static_cast<uint32_t>(sizeof route) };
    for (uint32_t i = 0; i < n_parts; ++i) frame[1 + i] = parts[i];
    const bool written = RingBufferWriter::write(
        base + L.nrt_out_ring_start, L.profile.nrt_out_ring_bytes,
        &cursors->nrt_out_head, &cursors->nrt_out_tail,
        &cursors->nrt_out_sequence, &e->nrt_write_lock,
        frame.data(), n_parts + 1, token);
    // Refused for want of room: counted, since the line logging it would
    // ride this same full ring (PerformanceMetrics::egress_frames_refused).
    if (!written) metrics->egress_frames_refused.fetch_add(1, std::memory_order_relaxed);
    if (written) {
        clockwork_control_wake(core);
        clockwork_core_said_bump(core);
    }
    return written;
}

extern "C" {

// ── Egress ──────────────────────────────────────────────────────────────────

// Both egress rings carry Message frames whose payload is [route:u32][osc];
// the drains peel the route word before the callback sees the OSC bytes.
// They walk from the ring's read cursor, as clockwork_client_poll does, and
// give the space back as soon as the walk ends (clockwork_ring_give_back).
static uint32_t drain_egress_ring(uint8_t* buffer, uint32_t size,
                                  std::atomic<int32_t>* head,
                                  std::atomic<int32_t>* tail,
                                  std::atomic<int32_t>* read,
                                  ClockworkDrainState& st,
                                  const ClockworkDrainMetrics& m,
                                  ClockworkEgressFn fn, void* ctx,
                                  uint32_t max_frames) {
    clockwork_ring_give_back(tail, read);   // a client's last batch, if it had one
    const uint32_t delivered = clockwork_drain_ring(buffer, size, head, read, st, m, max_frames,
        [fn, ctx](uint32_t sourceId, const uint8_t* payload, uint32_t n, uint32_t seq) {
            if (n >= EGRESS_ROUTE_SIZE) {
                uint32_t route;
                std::memcpy(&route, payload, sizeof(route));
                fn(ctx, sourceId, route,
                   payload + EGRESS_ROUTE_SIZE, n - EGRESS_ROUTE_SIZE, seq);
            }
            return ClockworkDrainVerdict::Consume;
        });
    clockwork_ring_give_back(tail, read);
    return delivered;
}

uint32_t clockwork_egress_rt_drain(ClockworkCore* core, ClockworkEgressFn fn, void* ctx, uint32_t max_frames) {
    clockwork::Egress* e = clockwork::egress(core);
    uint8_t* const base = clockwork::laid_out_base(core);
    if (!e || !base || !fn) return 0;
    RingCursors* const cursors = clockwork::cursors_in(base, clockwork::layout_of(core));
    PerformanceMetrics* const metrics = clockwork::metrics_in(base);
    // RT egress traffic counts into the segment-resident metrics so external
    // observers see reply/notification volume and ring health.
    ClockworkDrainMetrics m;
    m.received  = &metrics->osc_in_messages_received;
    m.bytes     = &metrics->osc_in_bytes_received;
    m.corrupted = &metrics->osc_in_corrupted;
    m.seqGaps   = &metrics->messages_sequence_gaps;
    const clockwork::Layout& L = clockwork::layout_of(core);
    return drain_egress_ring(base + L.rt_out_ring_start, L.profile.rt_out_ring_bytes,
                             &cursors->out_head, &cursors->out_tail, &cursors->out_read,
                             e->rt_drain, m, fn, ctx, max_frames);
}

uint32_t clockwork_egress_nrt_drain(ClockworkCore* core, ClockworkEgressFn fn, void* ctx, uint32_t max_frames) {
    clockwork::Egress* e = clockwork::egress(core);
    uint8_t* const base = clockwork::laid_out_base(core);
    if (!e || !base || !fn) return 0;
    RingCursors* const cursors = clockwork::cursors_in(base, clockwork::layout_of(core));
    PerformanceMetrics* const metrics = clockwork::metrics_in(base);
    // The same reply counters as the RT ring: which ring a reply rode is an
    // implementation detail (a guest's off-thread stage answers here, its
    // audio-thread stage there), and a client's "replies received" is the
    // sum. Sequence gaps stay per ring — this ring numbers its own frames.
    ClockworkDrainMetrics m;
    m.received  = &metrics->osc_in_messages_received;
    m.bytes     = &metrics->osc_in_bytes_received;
    m.corrupted = &metrics->osc_in_corrupted;
    const clockwork::Layout& L = clockwork::layout_of(core);
    const uint32_t delivered =
        drain_egress_ring(base + L.nrt_out_ring_start, L.profile.nrt_out_ring_bytes,
                          &cursors->nrt_out_head, &cursors->nrt_out_tail, &cursors->nrt_out_read,
                          e->nrt_drain, m, fn, ctx, max_frames);
    clockwork_egress_nrt_room_made(core);
    return delivered;
}

uint32_t clockwork_egress_nrt_max_frame(const ClockworkCore* core) {
    return nrt_egress_max(clockwork::layout_of(core)) - EGRESS_ROUTE_SIZE;
}

static uint32_t nrt_free(const RingCursors* cursors, const clockwork::Layout& L) {
    return ring_free_bytes(
        static_cast<uint32_t>(cursors->nrt_out_head.load(std::memory_order_acquire)),
        static_cast<uint32_t>(cursors->nrt_out_tail.load(std::memory_order_acquire)),
        L.profile.nrt_out_ring_bytes);
}

// The flag goes up before the second look, and a reader looks at it after it
// gives room back, so either the second look sees the room or the reader sees
// the flag.
void clockwork_egress_nrt_want_room(ClockworkCore* core) {
    clockwork::Egress* e = clockwork::egress(core);
    if (!e) return;
    e->nrt_room_wanted.store(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

int clockwork_egress_nrt_room(ClockworkCore* core, uint32_t bytes) {
    const RingCursors* cursors = clockwork::cursors_in(clockwork::laid_out_base(core), clockwork::layout_of(core));
    if (!clockwork::egress(core) || !cursors) return 0;
    const clockwork::Layout& L = clockwork::layout_of(core);
    if (nrt_free(cursors, L) >= bytes) return 1;
    clockwork_egress_nrt_want_room(core);
    return nrt_free(cursors, L) >= bytes;
}

uint32_t clockwork_egress_nrt_pending(ClockworkCore* core) {
    const RingCursors* cursors = clockwork::cursors_in(clockwork::laid_out_base(core), clockwork::layout_of(core));
    if (!clockwork::egress(core) || !cursors) return 0;
    const auto head = static_cast<uint32_t>(cursors->nrt_out_head.load(std::memory_order_acquire));
    const auto read = static_cast<uint32_t>(cursors->nrt_out_read.load(std::memory_order_acquire));
    return ring_used_bytes(head, read, clockwork::layout_of(core).profile.nrt_out_ring_bytes);
}

void clockwork_egress_nrt_room_made(ClockworkCore* core) {
    clockwork::Egress* e = clockwork::egress(core);
    if (!e) return;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (e->nrt_room_wanted.load(std::memory_order_relaxed) != 0
        && e->nrt_room_wanted.exchange(0, std::memory_order_relaxed) != 0)
        clockwork_control_wake(core);
}

bool clockwork_egress_nrt_write(ClockworkCore* core, uint32_t route, uint32_t token,
                         const uint8_t* osc, uint32_t len) {
    if (!osc || len == 0) return false;
    const RingPart part{ osc, len };
    return clockwork_egress_nrt_write_parts(core, route, token, &part, 1);
}

// ── Waiting for egress (lanes_internal.h) ───────────────────────────────────

int clockwork_egress_wait(ClockworkCore* core, uint32_t* seen, uint32_t timeout_ms) {
    const int moved = clockwork_core_said_wait(core, *seen, timeout_ms);
    if (moved >= 0) *seen = clockwork_core_said_value(core);
    return moved;
}

void clockwork_egress_wake(ClockworkCore* core) {
    clockwork_core_said_bump(core);
}

void clockwork_egress_count_undelivered(ClockworkCore* core, uint32_t n) {
    PerformanceMetrics* const metrics = clockwork::metrics_in(clockwork::laid_out_base(core));
    if (n && metrics) metrics->egress_frames_undelivered.fetch_add(n, std::memory_order_relaxed);
}

// ── Tick ────────────────────────────────────────────────────────────────────

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
ClockworkTick clockwork_tick(ClockworkCore* core, ClockworkBlockTime* time, uint32_t out_channels, uint32_t in_channels) {
    if (!time || time->struct_bytes < sizeof(ClockworkBlockTime)) return CLOCKWORK_TICK_NONE;
    // The block takes the core's gate and never waits for it: a control call
    // holding it means no block (lanes.h, CLOCKWORK_TICK_BUSY). The gate
    // first, the state under it: a core closing or closed has no block, and
    // one clockwork_init has not given its state has none yet.
    const clockwork::GateHold gate(core, CLOCKWORK_ENTRY_RENDER, clockwork::GateHold::Take::Try);
    if (gate.closed()) return CLOCKWORK_TICK_NONE;
    if (!gate.held()) return CLOCKWORK_TICK_BUSY;
    if (!clockwork::state(core)) return CLOCKWORK_TICK_NONE;
    process_audio(core, *time, out_channels, in_channels);
    // Once a block, and only for a block that said something: whoever waits
    // for egress (clockwork_client_wait) wakes when the block has ended, with
    // all of it in the ring. Two atomic operations and a fence when nobody
    // waits, as the control pass's own wake is.
    clockwork::Egress* e = clockwork::egress(core);
    const RingCursors* cursors = clockwork::cursors_in(clockwork::laid_out_base(core), clockwork::layout_of(core));
    if (e && cursors) {
        const int32_t seq = cursors->out_sequence.load(std::memory_order_relaxed);
        if (seq != e->rt_out_said) {
            e->rt_out_said = seq;
            clockwork_core_said_bump(core);
        }
    }
    return CLOCKWORK_TICK_RENDERED;
}

// ── Init ──────────────────────────────────────────────────────────────────────


void clockwork_init(ClockworkCore* core,
              double   sample_rate,
              uint32_t block_size,
              uint32_t input_channels,
              uint32_t output_channels,
              uint32_t verbosity,
              void*    guest_memory,
              uint32_t guest_memory_bytes,
              const void* guest_config,
              uint32_t    guest_config_bytes,
              uint32_t    arena_bytes,
              const void* assets,
              uint32_t    assets_bytes,
              void*       returns,
              uint32_t    returns_bytes) {
    if (!clockwork_core_live(core)) {
        clockwork_log(core, "[init] refused: not a live core (clockwork_core_new)");
        return;
    }
    // The core's own state, made at its first init.
    if (!clockwork::make_state(core)) {
        clockwork_log(core, "[init] refused: the core's state could not be made");
        return;
    }
#if CLOCKWORK_RUST_LOG && !defined(__EMSCRIPTEN__)
    // The Rust subsystems' lines go onto the ring of the core each names
    // (rust/clockwork-log). One stateless sink serves every core, so
    // installing it again is a no-op.
    clockwork_rust_log_install([](ClockworkCore* c, const char* line, uint32_t len) {
        clockwork_log_raw(c, line, len);
    });
#endif
    // No block renders while the core is built or comes back: one in flight
    // ends first, and the tick answers busy until this returns.
    const clockwork::GateHold gate(core, CLOCKWORK_ENTRY_CONTROL, clockwork::GateHold::Take::Wait);
    clockwork_wire_own_routes(core);

    // The guest's config block goes into the arena init_memory will read: the
    // host's segment when it gave the core one.
    const ClockworkConfig& cfg = *clockwork::config_of(core);
    uint8_t* const arena = cfg.segment ? cfg.segment : clockwork_local_arena(core);
#ifndef __EMSCRIPTEN__
    // Natively the arena is made here or not at all; on the web a host that
    // has not claimed the engine's memory yet gets it in init_memory.
    if (!arena) {
        clockwork_log(core, "[init] refused: no memory for the engine's arena (%u bytes)",
                      clockwork::layout_of(core).arena_bytes);
        return;
    }
#endif
#if CLOCKWORK_SYNTH
    // AN ARENA ALREADY UP IS AUDIO COMING BACK to the instance in it, as a
    // device returning is (docs/RESTARTS.md, "One event, every route"): a
    // page whose worklet the browser took away, reloaded over the same
    // memory; a host's own device change. It does the
    // least that gets the engine back where it was (come_back, below, once
    // the host's bytes are in place). With no block rendering, which is this
    // call's contract anyway.
    const bool coming_back = arena && clockwork::laid_out_base(core) == arena;
#endif

    // Copy the guest's bytes into the region reserved for them, without
    // looking at any of them. NULL means the host has already put them there
    // (the web one has no C pointer to pass) or that there are none.
    if (guest_config && guest_config_bytes) {
        // Refused, not cut short: a config that does not fit is one the host
        // and the region disagree about, and a truncated block boots a guest
        // that cannot tell it was cut mid-line. ClockworkEngine refuses the
        // same config before it gets here, and the web side throws on it
        // (writeGuestConfigToMemory); this is the backstop for a host that
        // calls clockwork_init itself.
        if (guest_config_bytes > GUEST_CONFIG_SIZE) {
            clockwork_log(core, "[init] refused: guest config is %u bytes but the region "
                    "holds %u — raise GUEST_CONFIG_SIZE (shared_memory.h) or send less",
                    guest_config_bytes, (uint32_t)GUEST_CONFIG_SIZE);
            return;
        }
        std::memcpy(arena + clockwork::layout_of(core).guest_config_start, guest_config, guest_config_bytes);
    }

    clockwork_set_engine_config(core, sample_rate, block_size, input_channels,
                           output_channels, verbosity,
                           guest_memory, guest_memory_bytes, arena_bytes,
                           assets, assets_bytes, returns, returns_bytes);
#if CLOCKWORK_SYNTH
    if (coming_back)
        come_back(core);
    else
#endif
        init_memory(core);
    // Where the core's arena is, so a client opened over it finds its core.
    if (uint8_t* base = clockwork::arena_base(core))
        clockwork_core_set_memory(core, base, arenaHeader(base)->arena_bytes);
}

// ── Setup ─────────────────────────────────────────────────────────────────────

namespace {
// What a host says of a core before it boots goes into the core's config,
// made with its state if this is the first thing said; null for a core that
// is not live.
ClockworkConfig* setup_of(ClockworkCore* core) {
    return clockwork::make_state(core) ? clockwork::config_of(core) : nullptr;
}
}

void clockwork_set_channel_ceilings(ClockworkCore* core, uint32_t input_channels, uint32_t output_channels) {
    if (ClockworkConfig* c = setup_of(core)) {
        c->input_channels  = input_channels;
        c->output_channels = output_channels;
    }
}

void clockwork_declare_fp_env(ClockworkCore* core, uint32_t env) {
    if (ClockworkConfig* c = setup_of(core)) c->fp_env = env;
}

void clockwork_set_guest_bulk_bytes(ClockworkCore* core, size_t bytes) {
    if (ClockworkConfig* c = setup_of(core)) c->guest_bulk_bytes = bytes;
}

void clockwork_set_deterministic_seed(ClockworkCore* core, int32_t seed) {
    if (ClockworkConfig* c = setup_of(core)) c->deterministic_seed = seed;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void clockwork_carry_generation(ClockworkCore* core, uint32_t generation) {
    if (ClockworkConfig* c = setup_of(core)) c->carried_generation = std::max(c->carried_generation, generation);
}

void clockwork_reserve_lanes(ClockworkCore* core, uint32_t lanes) {
    if (ClockworkConfig* c = setup_of(core))
        c->reserved_lanes = lanes < CLOCKWORK_RESERVED_LANES_MAX ? lanes : CLOCKWORK_RESERVED_LANES_MAX;
}

uint32_t clockwork_reserved_lanes(ClockworkCore* core) {
    const ClockworkConfig* c = clockwork::config_of(core);
    return c ? c->reserved_lanes : 0;
}

uint32_t clockwork_lane_base(ClockworkCore* core) {
    const clockwork::Build* b = clockwork::build(core);
    return b ? b->lane_base : 0;
}

void clockwork_reserve_link_audio_lanes(ClockworkCore* core, uint32_t lanes) {
    if (ClockworkConfig* c = setup_of(core))
        c->link_audio_lanes = lanes < CLOCKWORK_RESERVED_LANES_MAX ? lanes : CLOCKWORK_RESERVED_LANES_MAX;
}

uint32_t clockwork_link_audio_lane_base(ClockworkCore* core) {
    const clockwork::Build* b = clockwork::build(core);
    return b ? b->link_audio_lane_base : 0;
}

uint32_t clockwork_link_audio_lanes(ClockworkCore* core) {
    const ClockworkConfig* c = clockwork::config_of(core);
    return c && clockwork_link_audio_lane_base(core) ? c->link_audio_lanes : 0;
}

// ── Layout ──────────────────────────────────────────────────────────────────

const ClockworkArenaHeader* clockwork_arena_header(ClockworkCore* core) {
    uint8_t* const base = clockwork::arena_base(core);
    return base ? arenaHeader(base) : nullptr;
}

void* clockwork_lanes_base(ClockworkCore* core) {
    return clockwork::arena_base(core);
}

// ── The control ring (lanes.h) ──────────────────────────────────────────────

void clockwork_control_serve(ClockworkCore* core, int on) {
    RingCursors* const cursors = clockwork::cursors_in(clockwork::arena_base(core), clockwork::layout_of(core));
    if (!clockwork::ingress(core) || !cursors) return;
    if (on) cursors->status_flags.fetch_or(STATUS_CONTROL_SERVED, std::memory_order_release);
    else    cursors->status_flags.fetch_and(~static_cast<uint32_t>(STATUS_CONTROL_SERVED),
                                           std::memory_order_release);
}

int clockwork_control_served(ClockworkCore* core) {
    const RingCursors* cursors = clockwork::cursors_in(clockwork::arena_base(core), clockwork::layout_of(core));
    return clockwork::ingress(core) && cursors && (cursors->status_flags.load(std::memory_order_acquire) & STATUS_CONTROL_SERVED) ? 1 : 0;
}

// One pointer, so a writer always calls a function with its own context.
void clockwork_control_waker(ClockworkCore* core, const ClockworkControlWaker* waker) {
    if (clockwork::Ingress* in = clockwork::ingress(core))
        in->control_waker.store(waker, std::memory_order_release);
}

void clockwork_control_wake(ClockworkCore* core) {
    clockwork::Ingress* in = clockwork::ingress(core);
    if (!in) return;
    if (const ClockworkControlWaker* w = in->control_waker.load(std::memory_order_acquire))
        w->wake(w->ctx);
}

}  // extern "C"
