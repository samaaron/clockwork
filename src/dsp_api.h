// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * dsp_api.h — the contract between clockwork and a DSP.
 *
 * The DSP fills buffers and is handed messages with the time they are meant
 * for. What it does in between is its own affair, including whether it has a
 * graph, a node tree, buses, or a schedule at all.
 *
 * See docs/BOUNDARY.md.
 */
#ifndef CLOCKWORK_DSP_API_H
#define CLOCKWORK_DSP_API_H

#include <stdint.h>

#include "clockwork_event_sink.h"   /* ClockworkSink: where a DSP's own output goes */

struct ClockworkClockState;

#ifdef __cplusplus
extern "C" {
#endif

/* An instance. Opaque, with no C transcription of its fields anywhere, so
   nothing can drift. */
struct Dsp;

/* ── What clockwork gives the DSP ─────────────────────────────────────── */

typedef struct DspConfig {
    double   sample_rate;
    uint32_t block_size;        /* frames per dsp_process call */

    /* CEILINGS, not counts. The live counts arrive per block and change while
       running: Link peers join and leave, aux sinks are added, a device switch
       brings a different interface. Size buffers against these in dsp_new —
       the only place a DSP may allocate — and read the live counts per block.
       Clockwork guarantees the per-block counts never exceed them. */
    uint32_t max_input_channels;
    uint32_t max_output_channels;

    /* The session clock. Read it with readClockworkClock() (shared_memory.h),
       NEVER field by field — that is how the coherence rules get broken. May
       be NULL; readClockworkClock() then answers with a sane default. */
    const struct ClockworkClockState* clock;

    /* WHICH CHANNEL IS WHICH. Without this dsp_process is handed an anonymous
       range: a stereo file's left and right are indistinguishable from each
       other and from the microphone.

       Read it at the point of use — `channel_map->in[c]` is one relaxed load
       and tells you the kind (device, port, nothing) and which stream owns the
       channel. There is nothing to remember between blocks and nothing to
       consume: the read IS the lookup, and it is current by construction.

       The stream table beside it (names, ranges) is the COLD half, guarded by
       `generation`, and is not audio-thread work. See ClockworkChannelMapState
       in shared_memory.h.

       A channel is described no later than the audio it carries: the map entry
       is written before the binding is published. May be NULL when the host
       reserved no arena. */
    const struct ClockworkChannelMapState* channel_map;

    /* A window of shared memory the DSP may use however it likes. Clockwork
       publishes these bytes and never interprets them — with ONE exception,
       so that a client can poll it cheaply: the FIRST uint32 is a version
       stamp the guest bumps whenever it changes anything else in the window.
       Clockwork watches that word and nothing else; a guest that never bumps
       it is simply snapshotted on the usual interval, and one that publishes
       nothing may ignore the region entirely. NULL/0 if none. */
    void*    shm_window;
    uint32_t shm_window_bytes;

    /* Storage that OUTLIVES THIS INSTANCE, private to the DSP. Clockwork will
     * not touch it and will not zero it across a rebuild. A COLD BOOT DOES
     * clear it — only clockwork knows whether this is a device switch or a
     * fresh engine in a process that had one before.
     *
     * It exists because a DSP does not always get to say goodbye: a browser
     * kills the worklet outright when a tab is backgrounded, and there is no
     * thread left to run a save callback on.
     *
     * The bytes are hostile and must be treated so:
     *
     *   - TORN. A death mid-write leaves half an update. Stamp a generation
     *     and validate before believing anything here.
     *   - STALE. It may be from a run that crashed, and restoring blindly can
     *     resurrect the cause.
     *   - NOT DURABLE. Shared memory, not a file. It does not cross a process
     *     boundary, and a transport without shared memory has none of it.
     *
     * NULL/0 if none, which is a working state: a DSP given nothing restores
     * nothing and must still start. */
    void*    persistent;
    uint32_t persistent_bytes;

    /* ── The guest's three regions ────────────────────────────────────────
     *
     * EACH HAS EXACTLY ONE WRITER: the guest writes `arena` and `outbox`, the
     * client writes `inbox`. `inbox` is const here, so a GUEST that writes it
     * does not compile; the client holds the same bytes writable, and its own
     * view of `outbox` is const for the same reason.
     *
     * Bulk moves by not being copied. A client writes into `inbox` from its own
     * thread and sends a short message saying where; the guest receives an
     * offset rather than a payload. Nothing large crosses the audio thread in
     * either direction, which matters because every entry point in this header
     * is called on the audio thread — and on web that is forced, since the
     * guest exists only inside the AudioWorklet and no off-thread door can
     * exist.
     *
     * OFFSETS, NOT POINTERS, in any message naming a position in these. A
     * reader maps the segment wherever the kernel puts it, which is not the
     * address the engine sees, so a raw pointer names the wrong bytes on the
     * other side — and does so without faulting.
     *
     * A SEGMENT PUBLISHES THESE, IT DOES NOT CREATE THEM. A host with no
     * shared segment still hands over all three; a client in its own process
     * reads them in place. */

    /* The guest's own arena: working memory that needs no allocator to reach.
     * Clockwork reserves it, zeroes it at every dsp_new and never touches a
     * byte after that. NOTHING ELSE WRITES HERE.
     *
     * NULL only if the host provides none at all. That is not a licence to
     * allocate in dsp_process, which may not allocate either way — a guest
     * without an arena takes what it needs in dsp_new and holds it. */
    void*    arena;
    uint32_t arena_bytes;

    /* THE ARENA IS THE FAST TIER, AND THIS IS THE BULK ONE.
     *
     * A device with two kinds of RAM (the ESP32-S3: ~512 KiB of internal SRAM
     * behind 8 MiB of slower PSRAM) hands the guest two arenas: `arena` is
     * placed in the fast tier and `arena_bulk` in the bulk tier, so a guest can
     * carve its hot state — VM slots, kernel state, wires — from the one and its
     * cold, large state — loop cells, sample data, scope rings — from the other,
     * and the placement is decided by the host's memory profile rather than
     * inside each guest crate.
     *
     * Same rules as `arena`: the guest's alone, zeroed at every dsp_new, never
     * touched by clockwork after that.
     *
     * NULL/0 ON EVERY SINGLE-REGION HOST (desktop, web, the NIF): there is one
     * kind of memory, so there is nothing to place differently. A guest that
     * would have put something in bulk puts it in `arena` instead, and
     * clockwork has already checked `arena` can hold both wants (see
     * DspInfo::arena_bulk_bytes_wanted). Test `arena_bulk` for NULL, never the
     * platform. */
    void*    arena_bulk;
    uint32_t arena_bulk_bytes;

    /* Bulk in. THE CLIENT WRITES, THE GUEST ONLY READS — hence const. A guest
     * that needs to mutate what arrived copies it into `arena` first.
     *
     * A published range belongs to the guest until it says otherwise, so a
     * guest that reads in place rather than copying must say when it is done.
     * NULL/0 when the host offers no inbound bulk. */
    const void* inbox;
    uint32_t    inbox_bytes;

    /* Bulk out. THE GUEST WRITES, THE CLIENT ONLY READS. Write the payload
     * here and send a short message saying where it landed, rather than
     * pushing it through emit_osc, which would copy it in the audio callback.
     * NULL/0 when the host offers no outbound bulk. */
    void*    outbox;
    uint32_t outbox_bytes;

    /* The guest's own configuration, written by the host, opaque here — how
       many buses, how many nodes, how big a wire pool. Clockwork reserves the
       region, copies the block in, and hands over base and length without
       looking inside, so a change to the layout cannot break it. NULL/0 if the
       host supplies none. */
    const void* guest_config;
    uint32_t    guest_config_bytes;

    /* Pins the RNG seed for an offline render, so two runs of the same score
       agree. 0 for "seed however you like". */
    int32_t  deterministic_seed;

    /* THE FLOATING-POINT ENVIRONMENT OF THE THREAD dsp_process RUNS ON, as the
     * host declared it (DspFpEnv). Denormal flush-to-zero is a per-thread CPU
     * flag; the native device layer arms it on its audio thread, a web worklet
     * cannot arm it at all, and an embedded host may or may not. Two targets
     * that differ here render different tails from the same filter, so a guest
     * that promises the same samples on every target must not depend on the
     * flag — it reads this, and either flushes in software itself or keeps its
     * state out of the denormal range by construction.
     *
     * DSP_FP_ENV_UNKNOWN means the host declared nothing. Treat it as
     * DENORMALS_HONOURED: assuming a flush that is not there is the failure
     * that costs CPU; assuming none when there is one costs nothing. */
    int32_t  fp_env;
} DspConfig;

/* What DspConfig::fp_env can say. */
typedef enum DspFpEnv {
    DSP_FP_ENV_UNKNOWN            = 0,  /* the host declared nothing */
    DSP_FP_ENV_DENORMALS_HONOURED = 1,  /* IEEE denormals are computed (web, always) */
    DSP_FP_ENV_FLUSH_TO_ZERO      = 2   /* FTZ/DAZ armed on the audio thread by the host */
} DspFpEnv;

/* What the DSP may call back into. Every entry may be NULL; a DSP must check.
   `ctx` is clockwork's own and is passed back untouched. */
typedef struct DspHost {
    void* ctx;

    /* Send OSC out — a reply, a notification, anything. `origin` echoes the
     * value the message being answered arrived with, so clockwork can route it
     * back. Callable from the audio thread.
     *
     * 0 addresses the notification audience — the clients that asked to be
     * notified — one unicast per subscriber. That list is empty until someone
     * subscribes, and a client that sent a request is not on it by virtue of
     * having sent one, so a REPLY MUST CARRY THE ORIGIN IT WAS GIVEN. A
     * non-zero return means the frame was queued, never that anything
     * received it.
     *
     * BULK DOES NOT GO HERE: sending copies the payload into the egress ring,
     * so a large reply is a large memcpy in the audio callback. Write it into
     * DspConfig::outbox and send a short message saying where.
     *
     * Returns non-zero if accepted. A full ring DROPS and counts rather than
     * blocking, so a guest that sends bulk here gets a refusal it can see. */
    int (*emit_osc)(void* ctx, uint32_t origin, const uint8_t* bytes, uint32_t len);

    /* Diagnostics. `level` follows syslog severities (3 = error, 6 = info,
       7 = debug). NOT RT-SAFE; do not call from dsp_process. */
    void (*log)(void* ctx, int level, const char* text);

    /* Open a sink onto an external destination and get a handle. `target` names
     * it in the endpoint's own terms: a MIDI port name, or "host:port" for OSC.
     * `capacity` is clamped into the allowed range and rounded up to a power of
     * two, so you get at least what you asked for.
     *
     * Returns CLOCKWORK_SINK_NONE if the destination does not exist or this
     * build cannot reach it. CHECK IT: a sink that opened onto nothing would
     * accept every message and deliver none.
     *
     * CONTROL THREAD ONLY. It allocates and may touch the device, so it belongs
     * in dsp_new, never in dsp_process.
     *
     * GAP: a DSP usually learns which destination to open from a control
     * message, and dsp_osc is its only door for one — but dsp_osc runs on the
     * audio thread. So a DSP either opens in dsp_new from something it was
     * configured with, or opens on the audio thread and accepts an allocation
     * there. Closing this needs a per-instance control tick the boundary lacks.
     *
     * NOT IDEMPOTENT. Opening the same target twice gives two handles, two
     * sinks and two of the 16 slots clockwork remembers a DSP's sinks in;
     * after that this answers CLOCKWORK_SINK_NONE. The substrate cannot
     * usefully de-duplicate, because a sink belongs to whoever opened it and
     * clockwork closes a DSP's sinks when the DSP is freed — handing back a
     * sink another owner opened would let dsp_free close the engine's own MIDI
     * clock output. A DSP that opens on demand should therefore de-duplicate
     * the way clockwork does internally: list the open sinks and compare
     * clockwork_sink_target (clockwork_event_sink.h) before opening.
     *
     * Clockwork closes every sink a DSP opened when that DSP is freed. There is
     * deliberately no close here: a DSP's sinks last as long as the DSP. */
    ClockworkSink (*open_sink)(void* ctx, ClockworkSinkKind kind, const char* target,
                         uint32_t capacity);

    /* Send one message to a sink, delivered at `when` — an OSC timetag in the
     * same domain as dsp_process's block_time, 1 meaning immediately. A DSP
     * that knows a note is due 20 ms into the block says block_time + 20 ms and
     * stops thinking about it.
     *
     * Returns non-zero if accepted; a full sink DROPS and counts.
     *
     * CALLABLE FROM dsp_process. Never allocates, locks or blocks. */
    int (*send_sink)(void* ctx, ClockworkSink sink, const uint8_t* bytes,
                     uint32_t len, int64_t when);

    /* Give back bytes clockwork allocated. Clockwork owns the heap the guest's
       larger allocations come from and has to be told when one is finished
       with. Callable off the audio thread; freeing may defer internally.
       Passing NULL, or a pointer clockwork did not allocate, is ignored. */
    void (*free_bytes)(void* ctx, void* ptr);

    /* The guest is done with an asset it was handed through dsp_asset: the
     * slot in the inbox may be reclaimed. Clockwork tells the client that
     * committed it (/clockwork/asset/released <id>), and the client's pool
     * takes the slot back. Callable from dsp_process and dsp_osc — it emits
     * one short message and nothing else. Returns non-zero if the id was a
     * live asset; a release of something not committed is ignored. NULL on a
     * host with no asset facility. */
    int (*asset_release)(void* ctx, uint32_t id);
} DspHost;

/* ── What the DSP says about itself ─────────────────────────────────────── */

/* Vocabulary as data — the native counterpart of js/lib/dsp_profile.js. The C
   ABI stays fixed while each DSP declares its own words. */
typedef struct DspInfo {
    const char* name;
    const char* version;

    /* Non-zero if the DSP holds its own schedule and wants every message the
       moment it arrives, carrying its timetag. Zero to have clockwork hold
       timed messages and deliver them when due. */
    int32_t holds_schedule;

    /* WHAT THE DSP NEEDS FROM ITS ARENAS, in bytes, per tier. Static, like the
     * rest of this struct, because a guest that carves everything it will ever
     * hold at dsp_new knows the figure before any instance exists — a loop
     * cell of so many seconds, a voice pool of so many slots — and the host
     * should learn it before booting audio, not from a failed carve halfway
     * through dsp_new.
     *
     * Clockwork compares these with what it can offer and REFUSES dsp_new,
     * with a logged reason, when a non-zero want cannot be met in the tier it
     * names. It NEVER spills: a hot pool that silently landed in PSRAM is a
     * performance cliff with no error, which is worse than a boot that says
     * why it stopped. On a single-region host `arena` must hold BOTH wants,
     * and that is what is checked there; on a tiered host each want is checked
     * against its own tier.
     *
     * 0 means "no claim": take whatever is offered, including nothing. A guest
     * that allocates from the system in dsp_new (the historical native
     * arrangement) declares 0 and 0 and is unaffected. */
    uint32_t arena_bytes_wanted;
    uint32_t arena_bulk_bytes_wanted;

    /* Non-zero if the DSP wants the host's INBOUND EVENTS — what a MIDI port
     * or a game controller sent, as "/clockwork/midi/in/…",
     * "/clockwork/gamepad/in/…", and the "/clockwork/midi/ports" /
     * "/clockwork/gamepad/devices" pushes — handed to dsp_osc as they arrive,
     * on the audio thread, beside the clients that subscribed to them. The
     * one exception to the rule that nothing under "/clockwork/" reaches a
     * DSP, and made only because the DSP asked. Each carries the moment it
     * arrived as a trailing `t` argument when the host knows it (the web does;
     * a native port's arrival is the block it was drained in), so a DSP that
     * holds its own schedule can place it on its own timeline. Zero: a client
     * that wants the DSP to hear a keyboard relays it. Same host on both
     * sides: a DSP that sets this never learns which host it is on. */
    int32_t wants_events;

} DspInfo;

/* Static: describable before any instance exists, so a host can report what it
   is linked against without booting audio. */
const DspInfo* dsp_describe(void);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Build an instance, or NULL with `err` set to a message the DSP owns. */
struct Dsp* dsp_new(const DspConfig* config, const DspHost* host, const char** err);
void        dsp_free(struct Dsp* dsp);

/* ── The tick ───────────────────────────────────────────────────────────── */

/*
 * Render one block: read `frames` from each of `n_in` input channels, write
 * `frames` to each of `n_out` output channels. `in` may be NULL when the host
 * captures no input. Output is WRITTEN, NOT ACCUMULATED — a DSP with nothing to
 * say writes silence.
 *
 * THE COUNTS ARRIVE PER BLOCK and change while running. Neither exceeds the
 * ceiling in DspConfig.
 *
 * A channel index is a STABLE SLOT for the lifetime of the stream occupying it.
 * A peer leaving does not renumber the peers after it — its slot goes silent
 * and stays reserved until reused — because renumbering would silently hand a
 * DSP somebody else's audio on a channel it was already processing. Slots above
 * the live count are not written and must not be read.
 *
 * `block_time` is the OSC timetag of the block's FIRST frame (NTP 32.32). A
 * message due at T lands at frame (T - block_time) * sample_rate / 2^32, and
 * the DSP does that arithmetic because only the DSP knows what it means to act
 * at a frame.
 *
 * Called on the audio thread. No allocation, no locks, no IO.
 */
void dsp_process(struct Dsp* dsp,
                 const float* const* in, uint32_t n_in,
                 float* const* out, uint32_t n_out,
                 uint32_t frames,
                 int64_t block_time);

/* ── Messages ───────────────────────────────────────────────────────────── */

/*
 * Hand the DSP one OSC message or bundle.
 *
 * `when` is ALWAYS the message's own timetag; clockwork never flattens it to
 * "now". A DSP that declared holds_schedule receives messages as they arrive
 * and decides when to act; one that did not receives each during the block in
 * which it comes due. Either way the exact time survives, which is what makes
 * sub-block placement possible. OSC's own value 1 means "immediately" and is
 * kept.
 *
 * `origin` identifies the sender; pass it to DspHost::emit_osc unchanged.
 *
 * `block_time` is the timetag of the first frame of THE BLOCK THIS MESSAGE IS
 * FOR — the other half of dsp_process's placement arithmetic. Messages are
 * delivered before dsp_process renders the block they belong to, so a DSP that
 * used dsp_process's block_time instead would be a whole block out. A DSP that
 * does not place events within a block may ignore it.
 *
 * ON THE AUDIO THREAD. Both call sites are inside process_audio and there is no
 * other delivery path, so this must not allocate or block.
 *
 * `bytes` POINTS INTO THE INGRESS RING and is valid only for this call. A guest
 * that wants to keep a payload must copy it out before returning — so the
 * memcpy lands on the audio thread whatever its size. Clockwork having avoided
 * a copy moves who pays for it, not whether it is paid.
 *
 * A TIMED MESSAGE IS COPIED TWICE when the DSP does not hold its own schedule:
 * clockwork copies the payload into its data pool (SCHEDULER_DATA_POOL_SIZE) on
 * the audio thread during the drain, and the guest copies it again when it
 * fires.
 *
 * SO DO NOT SEND BULK THROUGH HERE. Write it into DspConfig::outbox and
 * send a short message saying where. What belongs here is commands, parameters,
 * notes, and references to bulk that lives elsewhere.
 */
void dsp_osc(struct Dsp* dsp,
             const uint8_t* bytes,
             uint32_t len,
             int64_t when,
             uint32_t origin,
             int64_t block_time);


/* ── Assets: bulk that enters through the inbox lane ─────────────────────── */

/*
 * A sample, a wavetable, a synthdef bundle, a model: a large blob the guest
 * needs in memory and must never receive through the message ring. It enters
 * through the inbox lane (DspConfig::inbox), and only the CLIENT puts it there:
 * the client owns the lane and a pool over it (clockwork_asset_pool.h), writes
 * the bytes into a slot — decoding a file first if that is what the bytes
 * need, with clockwork_audio_file.h, which is a client API — and sends one
 * message naming the slot:
 *
 *   /clockwork/asset/commit  ,iiiiiif  id kind offset bytes channels frames rate
 *
 * The audio thread checks the range against the lane and hands the guest a
 * pointer through dsp_asset. No copy: the guest reads the client's bytes where
 * they lie, for as long as it holds the asset. When it lets go it calls
 * DspHost::asset_release, and the client hears
 *
 *   /clockwork/asset/released  ,i  id
 *
 * and reclaims the slot. A commit that cannot be honoured is answered with
 *   /clockwork/asset/refused   ,is id reason
 * and the guest never sees it. A commit that could is answered with
 *   /clockwork/asset/committed ,i  id
 *
 * The id is the client's, and the guest's key to whatever it binds the bytes
 * to; committing an id that is still held is refused until it is released.
 * Nothing here knows what a sample is. The KIND says how to read the bytes,
 * and RAW says "however the guest likes".
 */
enum {
    CLOCKWORK_ASSET_RAW       = 0,   /* bytes; channels/frames/rate carry nothing */
    CLOCKWORK_ASSET_AUDIO_F32 = 1    /* interleaved 32-bit float frames; byte_count
                                      * == frames * channels * 4, and rate is Hz */
};

typedef struct ClockworkAsset {
    uint32_t    struct_bytes;   /* sizeof(ClockworkAsset), for the guest to check */
    uint32_t    id;
    uint32_t    kind;           /* CLOCKWORK_ASSET_* */
    uint32_t    origin;         /* who committed it; opaque to the guest */
    const void* bytes;          /* into the inbox; valid until the guest releases */
    uint32_t    byte_count;
    uint32_t    channels;       /* AUDIO_F32 only, else 0 */
    uint32_t    frames;
    float       sample_rate;
} ClockworkAsset;

/*
 * Take an asset. Called on the audio thread, between messages of the block the
 * commit arrived in; the range has been checked against the lane and, for
 * AUDIO_F32, the geometry against the byte count. Return 0 to take it — the
 * bytes stay valid until the guest calls DspHost::asset_release(id) — or
 * non-zero to refuse, which the client hears as /clockwork/asset/refused.
 * No allocation, no locks, no IO: binding a pointer is all this is for.
 */
int dsp_asset(struct Dsp* dsp, const ClockworkAsset* asset);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_DSP_API_H */
