// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/*
 * lanes.h — the clockwork engine boundary.
 *
 * The lowest-common-denominator C API between the engine core and every
 * host: one ingress lane (OSC in), two egress lanes (RT and NRT OSC out),
 * the per-block tick, and the self-describing arena layout for hosts that
 * address the rings directly.
 *
 * Pure C surface: no threads, no IO, no allocation, no JUCE, no
 * Emscripten. Hosts own their audio loop, their wakeups, and their
 * lifecycle; the engine owns everything behind this header.
 *
 * The engine is process-singleton: there is no engine handle, matching the
 * engine's global arena/ClockworkClock/ingress state.
 *
 * Wire format (ring/ring.h): every ring frame is a 16-byte Message
 * header {magic, length, sequence, sourceId} followed by the payload.
 * Egress payloads carry a leading route word (EgressRoute, shared_memory.h)
 * before the OSC bytes.
 *
 * ── Minimal host ───────────────────────────────────────────────────────────
 * A self-driven host (browser worklet, embedded, the test suite's fixture)
 * drives the engine through just: clockwork_init() once, then per block
 * clockwork_ingress_write() → clockwork_tick() → read clockwork_audio_out() → clockwork_egress_rt_drain().
 * That is now the whole contract for OSC as well as audio: clockwork_init publishes an
 * ingress root of its own — "/clockwork/" answered by clockwork, everything
 * else handed to the DSP — so a minimal host's messages reach the DSP with no
 * registration on its part. (They did not, until the boundary narrowing: the root
 * was registered only on worklet builds, and a lanes-only host had every
 * message it wrote drained, unrouted and dropped.) A full host that wants
 * control planes of its own publishes g_active_split after boot and
 * overrides it.
 *
 * A host defines no link-time globals for this: the region a DSP may publish
 * through is DspConfig::shm_window, filled in by clockwork at dsp_new().
 *
 * Beyond the ABI calls it must still supply:
 *   - the ClockworkClock composition root: link ClockworkClock.cpp +
 *     ClockworkClockNative.cpp + TimeSource.cpp + MidiTimelines.cpp, then pick the
 *     clock profile — define CLOCKWORK_WORKLET_CLOCK for the SAB-formula clock (the
 *     host publishes NTP anchors into the arena; the web and esp32s3 hosts do
 *     this — it also registers the inline /clock route, so link
 *     EngineClock.cpp too), or
 *     leave it undefined for the wall-clock+IIR native TimeSource (the
 *     native/JUCE host does this). The LinkSession header is an inline
 *     no-ops without CLOCKWORK_LINK, so no extra TU. Leave
 *     g_active_clockwork_clock null and the engine's clock/MIDI paths stay inert.
 * No freestanding reference host is built here, but the test suite drives
 * exactly this sequence: test/LanesFixture.cpp boots clockwork and ticks it
 * a block at a time through these calls alone, and test/test_audio_path.cpp
 * asserts on the samples that come out.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "clockwork_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default arguments are C++ only, but this header is included by C too (a C
 * host driving the engine). Spell the defaults through a macro that is empty
 * in C, so the same declarations compile both ways. */
#ifdef __cplusplus
#  define CLOCKWORK_ARG_DEFAULT(x) = x
#else
#  define CLOCKWORK_ARG_DEFAULT(x)
#endif

/* ── Ingress ───────────────────────────────────────────────────────────────
 * Write one complete OSC message or #bundle (wire format) into the IN
 * ring. Callable from ANY thread including the audio thread itself —
 * multi-producer, serialised by an UNBOUNDED spinlock (RingBufferWriter).
 * Holders only memcpy small frames, so the lock is normally held for
 * nanoseconds — but a writer preempted mid-write stalls every other
 * producer, audio thread included, until it is rescheduled. Returns false
 * when the ring is full (backpressure: the caller decides whether to
 * drop, retry, or count it).
 *
 * source_id is an opaque writer/origin token carried in the Message header
 * and surfaced on egress for reply routing. Web: 0 = main thread, 1+ =
 * workers. Native: the transport origin token (0 = in-process embedder).
 *
 * Messages written on the audio thread before clockwork_tick() are drained by
 * that same tick, subject to the per-block drain bound — so a co-resident
 * component running in the host's audio callback can emit OSC pre-tick and
 * have it performed in the same block.
 */
bool clockwork_ingress_write(const uint8_t* osc, uint32_t len, uint32_t source_id);

/* ── Egress ────────────────────────────────────────────────────────────────
 * Two single-consumer rings out of the engine:
 *
 *   RT  — written only inside clockwork_tick() on the audio thread (lock-free):
 *         replies to drained messages, whatever the DSP emits through
 *         DspHost, debug lines framed as /clockwork/debug.
 *   NRT — written by engine control threads (the OscEgress framer:
 *         EngineControl, MIDI, Link, device …) under the egress lock:
 *         async command replies, broadcasts.
 *
 * Each ring must have exactly one draining thread (they may be the same
 * thread — the native NRT gateway drains both). The callback receives one
 * frame at a time; route is an EgressRoute value, source_id the origin
 * token the reply targets, seq the ring sequence number (gap detection is
 * the caller's choice). Payload points into the ring itself (frames are
 * contiguous by wire invariant) and is valid only for the duration of the
 * call.
 *
 * Returns the number of frames delivered. max_frames == 0 means drain
 * everything available.
 */
typedef void (*ClockworkEgressFn)(void* ctx, uint32_t source_id, uint32_t route,
                           const uint8_t* osc, uint32_t len, uint32_t seq);

uint32_t clockwork_egress_rt_drain(ClockworkEgressFn fn, void* ctx, uint32_t max_frames);
uint32_t clockwork_egress_nrt_drain(ClockworkEgressFn fn, void* ctx, uint32_t max_frames);

/* The NRT egress *producer* (clockwork_egress_nrt_write) is engine-internal, not part
 * of this host boundary — hosts only drain egress. See lanes_internal.h. */

/* ── Tick ──────────────────────────────────────────────────────────────────
 * The per-block entry point. Audio thread/task only. One call:
 *   1. drains the ingress lane (bounded per block), classifying through the
 *      active boundary (clockwork_sys.h) — immediate execution (replies written to the
 *      RT egress lane as messages are performed), or the timed queue;
 *   2. fires events due in this block (each keeps its own timetag, so placing
 *      it inside the block is the DSP's arithmetic, not clockwork's);
 *   3. runs the DSP for one block;
 *   4. hands the rendered block to any hosted plugins and bound port sinks.
 *
 * ntp_now: on native builds, the NTP time (seconds since 1900) of this
 * block's first sample, used as-is. On WASM the exported clockwork_tick receives
 * AudioContext time instead and converts to NTP internally via ClockworkClock
 * (process_audio's CLOCKWORK_WORKLET_CLOCK branch, which the self-driven device
 * build takes too) — the argument's meaning is per-platform. Hosts without a
 * wall clock can pass base NTP + elapsed samples / sample rate.
 *
 * Input audio: write into clockwork_audio_in() before the call. Output audio:
 * read clockwork_audio_out() after it — clockwork_block_size() frames per channel,
 * channel-major, float. Sample-format conversion is host code.
 *
 * Returns false only on fatal engine error (host should stop calling).
 */
bool clockwork_tick(double ntp_now, uint32_t out_channels, uint32_t in_channels);

/* Sample-clock publication (sample position ↔ DAC time, consumed by scope
 * streams and any audible-time reader) lives on the clock subsystem:
 * ClockworkClock::publishSampleClock once per hardware callback, plus
 * advanceEngineFrames per rendered block. See
 * docs/PORTS.md. */

const float* clockwork_audio_out(void);   /* rendered block, channel-major     */
float*       clockwork_audio_in(void);    /* input bus region, fill before tick */
uint32_t     clockwork_block_size(void);  /* frames per block (web: 128)        */
double       clockwork_sample_rate(void); /* the DSP's rate; 0 before init      */
int64_t      clockwork_block_time(void);  /* the block in flight, in OSC time   */

/* ── Init ──────────────────────────────────────────────────────────────────
 * Bring the engine up: lay out the arena (rings, control, metrics, node-tree,
 * scope), allocate the RT-safe heap, and create the DSP. Call once before the
 * first clockwork_tick. Every host boots through this one call.
 *
 * All of it is Clockwork geometry — what the host opened, and how loud to be
 * about it. The guest's own configuration is not here and never was
 * clockwork's business; see guest_config below.
 *
 *   sample_rate      the device's rate, which always wins.
 *   block_size       frames per dsp_process call; 0 asks for the platform
 *                    default. Clamped into [32, kMaxBlockSize] — the staging
 *                    buffers are fixed-size, so a block that does not fit is
 *                    refused rather than rendered past the end of the arena.
 *                    Web must pass 128 or 0: AudioWorklet's render quantum is
 *                    fixed and the block has to equal it exactly.
 *   input_channels   ceilings, not live counts. The live counts arrive per
 *   output_channels  block through clockwork_tick, because they change while
 *                    running. Clamped to kMaxChannels.
 *   verbosity        0 keeps boot quiet; above 0 logs the memory map and
 *                    the arrangement it proved.
 *
 * The guest's arena (guest_arena, guest_arena_bytes) is where the guest may
 * allocate, and clockwork's promise about it is that nothing else will. Bulk
 * does not travel through it: `inbox` is written by the client and only read by
 * the guest, `outbox` the reverse, so no region has two writers. It is an ABSOLUTE address, not an offset from the arena — on web the
 * JS memory layout chooses it as a position in the wasm heap, and treating it
 * as arena-relative put it a whole ring_buffer_storage too high. (JavaScript
 * passes it as a plain number: a wasm32 pointer IS a number, which is why this
 * is a pointer here rather than the uint32 it was first written as — that
 * could not hold a native address at all.) Pass NULL/0 for "no reserved
 * region", which is what a host does when the guest claims its own memory from
 * the system, as the native host does.
 *
 * ── guest_config ───────────────────────────────────────────────────────────
 * Bytes for the GUEST, opaque to clockwork, which copies them into the
 * region it reserves and then hands the guest base and length
 * (DspConfig::guest_config). Nothing here reads a field, so the layout is a
 * private matter between a host and the guest it boots — see
 * src/native/GuestConfigBlock.h for the one this repository's native host
 * writes.
 *
 * Pass NULL/0 either because there is nothing to configure, or because the
 * bytes are already in place: a host that can write into the arena directly
 * may do so instead, and the web one must, because JavaScript has no C pointer
 * to hand over. The worklet fills the region over its SharedArrayBuffer and
 * then calls clockwork_init with NULL here.
 *
 * NRT/self-driven is implied — real-time off, memory-locking off.
 */
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
              const void* inbox        CLOCKWORK_ARG_DEFAULT(nullptr),
              uint32_t    inbox_bytes  CLOCKWORK_ARG_DEFAULT(0),
              void*       outbox       CLOCKWORK_ARG_DEFAULT(nullptr),
              uint32_t    outbox_bytes CLOCKWORK_ARG_DEFAULT(0));
/* arena_bytes: the span clockwork::mem allocates from, or 0 for the build-time
 * default. A host whose system allocator is safe to use (native) passes 0 and
 * nothing is claimed; a host where it is not (wasm) passes the size it
 * reserved in its own layout. The guest's real-time pool is taken from it, so
 * it has to be at least as large as the pool that guest will ask for. */

/* ── More than one host in a process ─────────────────────────────────────────
 *
 * clockwork_init() is the SOLE-HOST call. It boots the engine, or reconfigures
 * the running one — and reconfiguring is routine, not exceptional: a native
 * host calls it again on every device start and cold swap, because the device
 * is what decides the rate and the block.
 *
 * Which is exactly why a SECOND host must not use it. The engine is a process
 * singleton, and a plugin instantiated twice in one DAW, or two embedders in
 * one process, share the loaded library and therefore the engine. A second
 * clockwork_init() would quietly reconfigure the first host's engine
 * underneath it — not a crash, a takeover, and audible only as the first
 * host's settings changing for no reason.
 *
 * clockwork_attach() is the call for that case:
 *
 *   BOOTED  — nobody was attached; the engine now has the geometry you asked
 *             for, and you own it.
 *   JOINED  — an engine was already running and can serve you. YOUR ARGUMENTS
 *             WERE NOT APPLIED. Read what you actually got from
 *             clockwork_sample_rate() and clockwork_block_size().
 *   REFUSED — an engine is running that cannot serve you (a different rate or
 *             block, or fewer channels than you need). Nothing changed, and
 *             you are not attached.
 *
 * Geometry that can serve a joiner: the same sample rate, the same block size
 * (or 0, meaning "whatever is running"), and at least as many channels in each
 * direction as the joiner asked for. Rate and block are exact because they are
 * not negotiable per-caller — every host in the process renders the same
 * blocks at the same rate.
 *
 * clockwork_init() REFUSES while more than one host is attached, because at
 * that point there is no such thing as "the" host's geometry.
 *
 * Detach when done. The count is the whole protocol: there are no per-host
 * handles, because there is nothing per-host to hold. */
#define CLOCKWORK_ATTACH_REFUSED 0
#define CLOCKWORK_ATTACH_BOOTED  1
#define CLOCKWORK_ATTACH_JOINED  2

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
                const void* inbox        CLOCKWORK_ARG_DEFAULT(nullptr),
                uint32_t    inbox_bytes  CLOCKWORK_ARG_DEFAULT(0),
                void*       outbox       CLOCKWORK_ARG_DEFAULT(nullptr),
                uint32_t    outbox_bytes CLOCKWORK_ARG_DEFAULT(0));

/* Release one attachment. Harmless when nobody is attached. Does NOT tear the
 * engine down: clockwork has no shutdown, and a host that attaches again gets
 * the engine it left. */
void     clockwork_detach(void);

/* How many hosts are attached. 1 for every ordinary single-host build. */
uint32_t clockwork_attached(void);

/* Re-aim the DSP's channel ceilings for the NEXT rebuild.
 *
 * A device switch can change how many channels are open, and the rebuilt DSP
 * has to be built at the new count or audio written to a channel the boot-time
 * count did not cover lands on an internal bus instead of on hardware. Takes
 * effect at the next dsp_new, not on the running instance. Native hosts used
 * to reach into the config block for this, which is what made the block look
 * like clockwork state.
 */
void clockwork_set_channel_ceilings(uint32_t input_channels, uint32_t output_channels);

/* Declare the floating-point environment of the thread that will call
 * clockwork_tick / run the audio callback, for the NEXT build of the DSP.
 *
 * `env` is a DspFpEnv value (dsp_api.h): DENORMALS_HONOURED when nothing arms
 * a flush on that thread, FLUSH_TO_ZERO when the host does (the JUCE device
 * layer arms it on its audio thread every callback). It reaches the guest as
 * DspConfig::fp_env, and the guest reads it because two targets that differ
 * here render different filter tails from the same code.
 *
 * Only the host can know this — the flag is per thread and clockwork does
 * not own the thread — which is why it is declared rather than detected. A
 * host that says nothing leaves it UNKNOWN, which a guest treats as
 * DENORMALS_HONOURED. The web build ignores the declaration: a worklet cannot
 * arm a flush, so it is always DENORMALS_HONOURED there. Takes effect at the
 * next dsp_new, like clockwork_set_channel_ceilings. */
void clockwork_declare_fp_env(uint32_t env);

/* ── The host as the end of clockwork's chain ──────────────────────────────
 * A /clockwork/ verb the audio thread does not answer itself is, on a host
 * with no NRT thread, either REFUSED there or FORWARDED to the host: sent
 * back out over the egress to the origin that sent it, wrapped in a bundle
 * whose timetag is the call's time (1 for a verb that never waited), for a
 * front of the host's own to answer or refuse. The web client is such a
 * host — its main thread owns Web MIDI and the Gamepad API, so it answers
 * the /clockwork/midi/ and /clockwork/gamepad/ verbs itself — and the web
 * build forwards from the start. A native lanes host refuses until it says
 * otherwise. The liveness verbs (ping, echo, the clock snapshot) are answered
 * on the audio thread either way. Takes effect at once.
 *
 * The same switch makes the host the MIDI endpoint of every sink a guest
 * opens from then on (clockwork_event_sink.h, clockwork_sink_set_host_emit):
 * a guest's send goes out the same way, "/clockwork/midi/sink/send ,sb
 * <port> <bytes>" in a bundle carrying the send's time, for the host to put
 * on the port with that time. One fact, one switch: the host is the far end. */
void clockwork_host_forward(int on);
int  clockwork_host_forwards(void);

/* ── Reserved lanes ────────────────────────────────────────────────────────
 * Channels clockwork keeps ABOVE the device for its own routing — a hosted
 * plugin track's send and return (plugin_track.h) live here. The DSP is built
 * `lanes` channels wider than the device in BOTH directions, starting at a
 * stable base (32, or the first multiple of 8 above a wider device); the
 * device backend copies only the channels the device has, so nothing in the
 * block above ever reaches a speaker or arrives from a microphone.
 *
 * Set before clockwork_init, or before the cold swap that should apply it; read the
 * base after. A base of 0 means nothing is reserved — either nothing asked, or
 * the lanes did not fit under kMaxChannels, which is logged.
 */
#define CLOCKWORK_RESERVED_LANES_MAX 64
void     clockwork_reserve_lanes(uint32_t lanes);
uint32_t clockwork_reserved_lanes(void);
uint32_t clockwork_lane_base(void);

/* ── Layout ────────────────────────────────────────────────────────────────
 * The arena base pointer, for hosts that address the rings directly. The
 * arena's first bytes are its own table of contents (clockwork_arena.h):
 * a host or client with the base finds every region from there, and the
 * web runtime reads it at boot the same way. Hosts using the functions
 * above never need either.
 */
const ClockworkArenaHeader* clockwork_arena_header(void);
void*                       clockwork_lanes_base(void);

/* Which channel is which (ClockworkChannelMapState, shared_memory.h), for a host
   or an in-process client. A guest reads the same bytes through
   DspConfig::channel_map. NULL before clockwork_init. */
const struct ClockworkChannelMapState* clockwork_channel_map(void);

#ifdef __cplusplus
}
#endif
