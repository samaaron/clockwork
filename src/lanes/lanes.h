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
 * That is the whole contract for OSC as well as audio: clockwork_init publishes an
 * ingress root of its own — "/clockwork/" answered by clockwork, everything
 * else handed to the DSP — so a minimal host's messages reach the DSP with no
 * registration on its part. A full host that wants control planes of its
 * own publishes g_active_split after boot and overrides it.
 *
 * A host defines no link-time globals for this: the region a DSP may publish
 * through is DspConfig::shm_window, filled in by clockwork at dsp_new().
 *
 * Beyond the ABI calls it must still supply:
 *   - the ClockworkClock composition root: link ClockworkClock.cpp +
 *     ClockworkClockNative.cpp + TimeSource.cpp + MidiTimelines.cpp, then pick the
 *     clock profile — define CLOCKWORK_WORKLET_CLOCK when the engine's clock
 *     is the only clock in the process (the web and esp32s3 hosts): the clock
 *     reads 0 until the first block anchors it, and the inline /clock route
 *     is registered, so link EngineClock.cpp too. Leave it undefined when the
 *     host has a wall clock the engine can read before the first block (the
 *     native/JUCE host). The LinkSession header is an inline
 *     no-ops without CLOCKWORK_LINK, so no extra TU. Leave
 *     g_active_clockwork_clock null and the engine's clock/MIDI paths stay inert.
 * No freestanding reference host is built here, but the test suite drives
 * exactly this sequence: test/LanesFixture.cpp boots clockwork and ticks it
 * a block at a time through these calls alone, and test/test_audio_path.cpp
 * asserts on the samples that come out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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
 * Each ring must have exactly one draining thread at a time (it may be the
 * same thread for both). These drains and clockwork_client_poll move the
 * same read cursor, so a host may hand the stream from one to the other and
 * the one taking over starts where the other stopped. The callback receives
 * one frame at a time; route is an EgressRoute value, source_id the origin
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
 * The block's time (ClockworkBlockTime): inputs from the host, outputs from
 * the engine. No host keeps a clock of its own: the engine's TimeSource
 * (clock/TimeSource.h) is the one clock on native, in the worklet and in an
 * embedded host, and the tick anchors it, slews it, publishes it and records
 * its history. Inputs: audio_frames, the host's count of frames rendered so
 * far, i.e. this block's first frame (the device callback's sample position,
 * AudioContext time in frames, an embedded host's rendered count); wall_ntp,
 * the host's wall clock now in NTP seconds (the callback's, Date.now(), an
 * embedded host's own); monotonic_secs, the host's monotonic clock now, or 0
 * if it has none (the worklet); output_latency_frames, the device's output
 * latency, 0 without a device; link_host_micros, the audio framework's
 * playback timestamp for this block in Link's clock (CoreAudio supplies
 * one), 0 otherwise. Outputs: block_ntp, the engine's NTP time for the
 * block's first frame; block_host_micros, the block's Link stamp, the host's
 * own when it gave one, else the engine's. A host with no wall clock (a
 * manual pump) passes its own time as wall_ntp and 0 as monotonic_secs and
 * never sees a gap. The engine detects gaps from these inputs once per block
 * (clockwork_audio_back).
 *
 * Input audio: write into clockwork_audio_in() before the call. Output audio:
 * read clockwork_audio_out() after it — clockwork_block_size() frames per channel,
 * channel-major, float. Sample-format conversion is host code.
 *
 * Returns false only on fatal engine error (host should stop calling).
 */
typedef struct ClockworkBlockTime {
    uint32_t struct_bytes;          /* sizeof(ClockworkBlockTime): the struct may grow, a host says which it built */
    uint32_t output_latency_frames; /* in: what the device plays a rendered frame after; 0 with no device */
    uint64_t audio_frames;          /* in: the host's own count of audio rendered: this block's first frame */
    double   wall_ntp;              /* in: the host's wall clock now, NTP seconds */
    double   monotonic_secs;        /* in: the host's monotonic clock now, seconds from any origin; 0 where it has none */
    uint64_t link_host_micros;      /* in: the framework's playback stamp for the block in Link's clock; 0 where it gives none */
    double   block_ntp;             /* out: the engine's time for the block's first frame, NTP seconds */
    uint64_t block_host_micros;     /* out: the block's stamp in Link's clock (0 without Link) */
} ClockworkBlockTime;
bool clockwork_tick(ClockworkBlockTime* time, uint32_t out_channels, uint32_t in_channels);

/* THE AUDIO WAS AWAY? The one rule on every target. The gap is the time that
 * passed since the last block less the audio rendered in it, by the host's
 * monotonic clock where it has one (a system clock set by hand is not a gap)
 * and by its wall clock otherwise, against the record of the last block in
 * the arena (so a worklet made again over the same memory, or an embedded
 * host attaching again, sees it). A host asks once a block as it hands its
 * time over, BEFORE its clock moves, with its own count of the audio rendered
 * (audio_secs, seconds from any origin): true when the gap is over
 * kClockStepSeconds, and the tick re-anchors the clock at this block
 * (resetAudioThreadTime). Nothing queued is dropped for a gap: it is
 * delivered, late by the gap; a host that wants it gone says so
 * (clockwork_ingress_mark_stale). A host asks clockwork_audio_starts as its
 * audio starts —
 * a device opened, an embedded host attached, a worklet made — judged by time
 * alone, as nothing was rendered meanwhile; its first block after is never a
 * gap, as its count of the audio starts over with it. */
bool clockwork_audio_back(double wall_ntp, double monotonic_secs, double audio_secs);
bool clockwork_audio_starts(double wall_ntp, double monotonic_secs);

/* The latest a timed event held by clockwork's scheduler may play: one that
 * comes due more than `ms` after its time is dropped instead, counted
 * (scheduler_late_dropped) and said once in a while. 0, the default, allows
 * any lateness: nothing is ever dropped for it, and an event plays late
 * (scheduler_lates). A host's policy for an engine that can be flooded
 * (an interactive page) — never clockwork's own. Set as the engine comes
 * up; read on the audio thread. */
void clockwork_set_timed_event_late_limit_ms(uint32_t ms);

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
 * scope), reserve the guest's memory and run its pool, and create the DSP. Call once before the
 * first clockwork_tick. Every host boots through this one call.
 *
 * All of it is Clockwork geometry — what the host opened, and how loud to be
 * about it. The guest's own configuration is not clockwork's business; see
 * guest_config below.
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
 * The guest's arena (guest_memory, guest_memory_bytes) is where the guest may
 * allocate, and clockwork's promise about it is that nothing else will. Bulk
 * does not travel through it: `assets` is written by the client and only read by
 * the guest, `returns` the reverse, so no region has two writers. It is an ABSOLUTE address, not an offset from the arena — on web the
 * JS memory layout chooses it as a position in the wasm heap, and an
 * arena-relative reading lands a whole arena_storage too high. (JavaScript
 * passes it as a plain number: a wasm32 pointer IS a number, which is why this
 * is a pointer rather than a uint32 — a uint32 could not hold a native
 * address.) Pass NULL/0 for "no reserved region", which is what a host does
 * when the guest claims its own memory from the system, as the native host
 * does.
 *
 * ── guest_config ───────────────────────────────────────────────────────────
 * Bytes for the GUEST, opaque to clockwork, which copies them into the
 * region it reserves and then hands the guest base and length
 * (DspConfig::guest_config). Nothing here reads a byte, so the layout is a
 * private matter between a host and the guest it boots — the hosts in this
 * repository write `name=value` lines (src/native/GuestConfigText.h).
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
              void*    guest_memory,
              uint32_t guest_memory_bytes,
              const void* guest_config,
              uint32_t    guest_config_bytes,
              uint32_t    arena_bytes,
              const void* assets        CLOCKWORK_ARG_DEFAULT(nullptr),
              uint32_t    assets_bytes  CLOCKWORK_ARG_DEFAULT(0),
              void*       returns       CLOCKWORK_ARG_DEFAULT(nullptr),
              uint32_t    returns_bytes CLOCKWORK_ARG_DEFAULT(0));
/* arena_bytes: the span clockwork::mem allocates from, or 0 for the build-time
 * default. A host whose system allocator is safe to use (native) passes 0 and
 * nothing is claimed; a host where it is not (wasm) passes the size it
 * reserved in its own layout. The guest's real-time pool is taken from it, so
 * it has to be at least as large as the pool that guest will ask for. */

/* ── More than one host in a process ─────────────────────────────────────────
 *
 * clockwork_init() is the SOLE-HOST call. It boots the engine, or brings back
 * the one running in the arena that is up — and that is routine, not
 * exceptional: a page reloaded over its memory, a host attaching again, a
 * device that decides another rate or block. It does the least that gets the
 * engine back where it was (docs/RESTARTS.md): the instance carries on, is
 * told what changed, or is built again.
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
                void*    guest_memory,
                uint32_t guest_memory_bytes,
                const void* guest_config,
                uint32_t    guest_config_bytes,
                uint32_t    arena_bytes,
                const void* assets        CLOCKWORK_ARG_DEFAULT(nullptr),
                uint32_t    assets_bytes  CLOCKWORK_ARG_DEFAULT(0),
                void*       returns       CLOCKWORK_ARG_DEFAULT(nullptr),
                uint32_t    returns_bytes CLOCKWORK_ARG_DEFAULT(0));

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
 * effect at the next dsp_new, not on the running instance.
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

/* A floor under the guest's bulk memory (DspConfig::memory_bulk, the span
 * its allocations come from) for the NEXT build.
 *
 * The span is sized before dsp_new as the largest of what the guest
 * declares (DspInfo::memory_bulk_bytes_wanted, dsp_memory_bulk_bytes), the
 * memory profile's default and this. A host that knows its session will
 * allocate more than the guest can foresee says so here. 0 restores the
 * profile's default (CLOCKWORK_GUEST_MEMORY_BULK_BYTES). A span the system
 * cannot provide fails the build with that reason (clockwork_boot_error),
 * rather than booting a guest that cannot allocate. On a tiered or
 * arena-bound host (web, embedded) the figure is a ceiling: the span is
 * still clamped to the memory that tier has, and only the guest's own want
 * is refused rather than clamped. */
void clockwork_set_guest_bulk_bytes(size_t bytes);

/* Why the last build has no guest, or NULL when it has one: the first thing
 * that stopped the build, or the guest's own reason when dsp_new refused. Text
 * for a person, stable until the next build. A host reports it to its clients
 * (the native engine enters its error state with it as the reason) rather than
 * running on as though the guest were there. */
const char* clockwork_boot_error(void);

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
 * on the port with that time. */
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

/* Link Audio's lanes, directly above the host's, reserved the same way. A
 * peer's channel arrives on a pair of them, and the engine hands out the pairs
 * (/clockwork/clock/audio/input/add): a client cannot choose, because it does
 * not know where they are, and they move up when a wider device arrives. The
 * host's lanes keep the base. Both are 0 when none were reserved or they did
 * not fit. */
void     clockwork_reserve_link_audio_lanes(uint32_t lanes);
uint32_t clockwork_link_audio_lanes(void);
uint32_t clockwork_link_audio_lane_base(void);

/* ── Layout ────────────────────────────────────────────────────────────────
 * The arena base pointer, for hosts that address the rings directly. The
 * arena's first bytes are its own table of contents (clockwork_arena.h):
 * a host or client with the base finds every region from there, and the
 * web runtime reads it at boot the same way. Hosts using the functions
 * above never need either.
 */
const ClockworkArenaHeader* clockwork_arena_header(void);
void*                       clockwork_lanes_base(void);

/* Whether the lanes have a reset for a region of the table: every region but
 * the ones that live as long as the arena has one, run at the event its
 * lifetime in the table names — a rebuild, or a cold boot (docs/RESTARTS.md,
 * "Every region, at every event"). The table decides WHEN; this is only
 * "is there a HOW", for a test that holds the two to each other. 0 for a
 * region the table does not list. */
int clockwork_region_has_reset(uint32_t region_id);

/* ── The control ring ──────────────────────────────────────────────────────
 * Clockwork's own CONTROL verbs — notify, devices, the session tempo, a
 * subsystem's verbs: everything of clockwork's the audio thread does not
 * perform inside its block (clockwork_sys_is_audio_verb) — have a ring of
 * their own in the arena (CONTROL_RING_START, lifetime arena), so that the
 * pass answering them has no audio block between it and the verb: a stopped
 * device, a stalled callback or a reopen blocked inside a driver cannot
 * block the engine's control replies (docs/RESTARTS.md, "The control ring").
 *
 * The engine SERVES it: it says so here once its control pass drains the
 * ring, and the word (STATUS_CONTROL_SERVED in the cursors region) is what
 * every boundary reads — clockwork_ingress_write, clockwork_client_send in
 * any process — to write a control verb there rather than on the in ring.
 * Until it is said, and in a build with no control pass, every verb rides
 * the in ring and the audio thread forwards control along its chain.
 * Lowered at shutdown. */
void clockwork_control_serve(int on);
int  clockwork_control_served(void);

/* ── What the guest instance survives (dsp_api.h, DspInfo::survives) ──────
 * The kinds of device change the linked guest takes live, read once at boot
 * from dsp_describe; 0 for a guest that survives nothing, and before a
 * guest is described. The engine plans a swap by it (DevicePolicy). */
uint32_t clockwork_guest_survives(void);

/* Tell the instance a device change it declared it survives: the new rate
 * and channel ceilings, with audio stopped. Clockwork's own regions that
 * follow the device — the scope's format, the sample clock, the channel
 * map's device words — are laid out again for it; nothing else moves, and
 * the instance keeps everything it holds. 1 if the instance took it; 0 if
 * it refused, or there is no instance, on which the caller does what it
 * would have done otherwise: a cold restart. */
int clockwork_guest_reconfigure(double sample_rate, uint32_t input_channels,
                                uint32_t output_channels);

/* Which channel is which (ClockworkChannelMapState, shared_memory.h), for a host
   or an in-process client. A guest reads the same bytes through
   DspConfig::channel_map. NULL before clockwork_init. */
const struct ClockworkChannelMapState* clockwork_channel_map(void);

#ifdef __cplusplus
}
#endif
