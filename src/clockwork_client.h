/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_client.h — the contract between clockwork and a client.
 *
 * dsp_api.h is the other side of the engine: what a guest implements. This is
 * what a CLIENT gets — anything that drives clockwork from outside the audio
 * thread. A GUI, a language binding, a test harness, a CLI, a browser tab
 * calling this compiled to WebAssembly. They differ in language and in where
 * they run; they do not differ in what they need, which is why this is one
 * header rather than one per consumer.
 *
 * Everything a client does is here: put a message in, take messages out, and
 * look at the regions the engine publishes. Nothing above that — no notion of
 * a synth, a node, a sample or a track. Those belong to whatever guest is
 * loaded, and a client that knows about them knows it on its own account.
 *
 *
 * ── WHAT THIS REPLACES ──────────────────────────────────────────────────────
 *
 * Every consumer has so far written the ring arithmetic itself: the sequence
 * numbers, the wrap, the padding marker, the alignment, the read-head dance
 * against a producer that never blocks. That is a page of subtle code, it was
 * written at least three times, and a mistake in it does not fail cleanly — it
 * yields a torn message, or a reader that stalls, at a moment nobody can
 * reproduce. Once is enough.
 *
 *
 * ── TRANSPORTS ──────────────────────────────────────────────────────────────
 *
 * A client reaches an engine one of several ways, and the handle is the same
 * afterwards. Shared memory is the first, not the definition:
 *
 *   open_shm      another process on this machine, through the POSIX segment
 *   open_memory   the same address space — a plugin's GUI, or a WebAssembly
 *                 build where "the segment" is the wasm heap
 *
 * A socket transport belongs here too and is not written yet. When it is, it
 * adds an open_* and changes nothing else: a caller that holds a handle does
 * not know which one it opened, and regions it cannot reach report absent
 * rather than failing the call.
 *
 * THE ENGINE'S OWN TRANSPORTS ARE CLIENTS OF THIS, and that is the point of
 * putting it here rather than in a helper library beside the GUI. A UDP
 * datagram from a machine across the room does not enter the engine by a
 * private door: the receiving thread mints an origin token for the sender and
 * calls clockwork_client_send with it, exactly as an in-process GUI does. One
 * ingress path, one implementation of the ring arithmetic, and a bug in it is
 * a bug everywhere rather than a bug in whichever copy was written last.
 *
 *
 * ── THREADS ─────────────────────────────────────────────────────────────────
 *
 * Every call here is for a CLIENT thread. None of it is real-time safe and
 * none of it belongs on an audio thread — a client does not have one, and the
 * engine's own is on the other side of the boundary.
 *
 * One handle is for one thread. Two threads that both want to talk to the
 * engine open two handles; that is cheap and it is what the ingress ring's
 * multi-producer lock is for. The exception is deliberate and stated at
 * clockwork_client_poll: draining is single-consumer, so exactly one thread
 * may poll a given handle's egress.
 *
 *
 * ── MEMORY ──────────────────────────────────────────────────────────────────
 *
 * This library allocates on open and frees on close, and not otherwise. Poll
 * fills an array the caller owns; regions hand back pointers into the mapping
 * rather than copies. A client that wants copies makes them, knowing what it
 * is paying — the header will not decide that for it.
 */
#ifndef CLOCKWORK_CLIENT_H
#define CLOCKWORK_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Versioning ──────────────────────────────────────────────────────────────
 *
 * The number this header describes. clockwork_client_abi_version() returns
 * what the library was built as, so a binding loaded against a different
 * library can refuse rather than misread a struct.
 *
 * Structs here grow by APPENDING, never by reordering, and every one that may
 * grow carries its own `struct_bytes` as its first field: a caller sets it to
 * sizeof(its version), and the library fills what fits and leaves the rest.
 * That is what lets a new library serve an old client and the reverse.
 */
#define CLOCKWORK_CLIENT_ABI_VERSION 1u

uint32_t clockwork_client_abi_version(void);

/* ── Status ──────────────────────────────────────────────────────────────────
 *
 * Every call that can fail says why. There is no errno, no last-error, and no
 * status hidden on the handle: a thread's failure is returned to that thread.
 */
typedef enum ClockworkStatus {
    CLOCKWORK_OK = 0,

    CLOCKWORK_E_ARG,        /* a null pointer, a zero length, a bad enum */
    CLOCKWORK_E_VERSION,    /* the engine's layout is not one this build reads */
    CLOCKWORK_E_NOT_FOUND,  /* no engine at that address/key */
    CLOCKWORK_E_PERM,       /* found, but not ours to open */
    CLOCKWORK_E_ABSENT,     /* the engine has no such region (see the note on
                             * regions: absent is an answer, not a fault) */
    CLOCKWORK_E_FULL,       /* the ingress ring had no room; try again later */
    CLOCKWORK_E_TOO_BIG,    /* the message cannot fit in the ring at any moment */
    CLOCKWORK_E_CLOSED,     /* the engine went away under us */
    CLOCKWORK_E_NOMEM
} ClockworkStatus;

/* A phrase for a status, for logs and for bindings that want to raise. Static
 * storage, never null, never allocated. */
const char* clockwork_client_status_text(ClockworkStatus s);

/* ── The handle ──────────────────────────────────────────────────────────── */

typedef struct ClockworkClient ClockworkClient;

/* Attach to an engine in another process on this machine.
 *
 * `endpoint` is the engine's attach endpoint: the Unix socket (macOS/Linux) or
 * named pipe (Windows) it serves its segment from — what it was started with
 * as --shm-endpoint, or clockwork_client_default_endpoint(port) when it was
 * not. The segment itself has no name; the engine hands over a duplicate of
 * its handle on connect, and only to a client running as the same user. An
 * engine started with -u 0 has no segment and no endpoint; that, an engine
 * that is not there, and one whose layout this build does not recognise are
 * all CLOCKWORK_E_NOT_FOUND. */
ClockworkClient* clockwork_client_open_shm(const char* endpoint, ClockworkStatus* out_status);

/* The endpoint an engine on `port` serves at unless told otherwise: under
 * XDG_RUNTIME_DIR or TMPDIR on POSIX, \\.\pipe\clockwork-shm-<port> on
 * Windows. Written NUL-terminated into `buf`; returns the length it needed,
 * so a result >= `cap` means the buffer was too small and nothing usable was
 * written. */
uint32_t clockwork_client_default_endpoint(uint32_t port, char* buf, uint32_t cap);

/* Attach through a handle already received — an fd from a Unix socket, a
 * HANDLE duplicated into this process — for a host that does the hand-off
 * itself. Owned by the client from here, whether or not the open succeeds. */
ClockworkClient* clockwork_client_open_shm_handle(intptr_t native_handle,
                                                  ClockworkStatus* out_status);

/* Attach to an engine in THIS address space: a plugin whose GUI is in the same
 * process, or a WebAssembly build where the engine's arena is the wasm heap.
 *
 * `base` is the engine's arena base and `bytes` its length. Nothing is mapped
 * and nothing is owned — the caller guarantees the memory outlives the handle. */
ClockworkClient* clockwork_client_open_memory(void* base, uint32_t bytes,
                                              ClockworkStatus* out_status);

/* How much storage a handle needs, for the placement open below. Constant for
 * a given build; ask rather than hard-code it. */
size_t clockwork_client_sizeof(void);

/* Open into storage the CALLER owns, allocating nothing.
 *
 * For anyone who cannot or will not have this library call malloc:
 *
 *   - a second wasm instance sharing one heap with the engine, where the
 *     allocator is not thread-safe and two callers into it corrupt it
 *     intermittently rather than loudly;
 *   - an embedded target with no heap at all;
 *   - a caller that pools its handles.
 *
 * `storage` must be at least clockwork_client_sizeof() bytes and aligned for
 * a pointer. It must outlive the handle. Close is still required — it
 * releases whatever the handle attached to — but it will not free `storage`.
 */
ClockworkClient* clockwork_client_open_memory_in(void* storage, size_t storage_bytes,
                                                 void* base, uint32_t bytes,
                                                 ClockworkStatus* out_status);

/* Release the handle. Safe on NULL. Regions handed out by this handle are
 * invalid afterwards. A handle opened in caller storage is detached, not
 * freed. */
void clockwork_client_close(ClockworkClient* c);

/* ── What is on the other end ────────────────────────────────────────────────
 *
 * Filled by clockwork_client_info(). Grows by appending; set struct_bytes.
 */
typedef struct ClockworkClientInfo {
    uint32_t struct_bytes;      /* in: sizeof(ClockworkClientInfo) as you know it */

    uint32_t abi_version;       /* the library's, for a binding that wants to check */
    uint32_t engine_version;    /* the engine's own build version */
    uint32_t features;          /* CLOCKWORK_FEATURE_* — what this engine has */

    double   sample_rate;
    uint32_t block_frames;
    uint32_t input_channels;
    uint32_t output_channels;
} ClockworkClientInfo;

/* Feature bits. A client asks rather than assumes: an engine built for an
 * embedded target may have no scope, no taps and no bulk lanes, and a client
 * that renders a scope should grey the control rather than read zeros. */
#define CLOCKWORK_FEATURE_SCOPE      (1u << 0)
#define CLOCKWORK_FEATURE_AUDIO_TAPS (1u << 1)
#define CLOCKWORK_FEATURE_BULK       (1u << 2)   /* inbox + outbox */
#define CLOCKWORK_FEATURE_WINDOW     (1u << 3)   /* the guest publishes one */
#define CLOCKWORK_FEATURE_SCHEDULER  (1u << 4)   /* timed messages are held */

ClockworkStatus clockwork_client_info(ClockworkClient* c, ClockworkClientInfo* out);

/* ── Sending ─────────────────────────────────────────────────────────────────
 *
 * One OSC message or bundle, copied into the ingress ring. Returns as soon as
 * it is queued: the engine reads it on its next block.
 *
 * `origin` is the client's own token, echoed back on any reply so a client
 * with several conversations in flight can tell them apart. Zero is legal and
 * means "I am not distinguishing" — it is NOT a broadcast, and a reply to it
 * goes to the engine's notification audience rather than to the sender.
 *
 * CLOCKWORK_E_FULL is a moment, not a verdict: the ring drains every block, so
 * a caller retries. CLOCKWORK_E_TOO_BIG is a verdict — the message is larger
 * than the ring can ever hold, and bulk belongs in the inbox instead.
 */
ClockworkStatus clockwork_client_send(ClockworkClient* c,
                                      const uint8_t* osc, uint32_t bytes,
                                      uint32_t origin);

/* ── Sending without a buffer of your own ────────────────────────────────────
 *
 * clockwork_client_send takes bytes that already exist somewhere. A caller
 * whose bytes DO NOT yet exist anywhere — an encoder that is about to build
 * the message — would otherwise have to build it into a buffer of its own and
 * have this copy it in. These three let it build the message in the ring, in
 * the place the message is going to live.
 *
 * That matters most where the two buffers are not even the same kind of
 * memory. A WebAssembly client's encoder writes to the JavaScript heap, which
 * the engine cannot address at all, so every message had to be copied into a
 * staging buffer inside the shared memory before it could be sent. Encoding
 * into the reservation removes both the staging buffer and the copy.
 *
 *     uint8_t* p = clockwork_client_send_begin(c, upper_bound, &st);
 *     if (p) {
 *         uint32_t n = encode_into(p, upper_bound);
 *         clockwork_client_send_commit(c, n, origin);
 *     }
 *
 * BEGIN HOLDS THE RING'S WRITE LOCK UNTIL COMMIT OR ABORT. No other producer
 * can write while a reservation is open, so what happens in between must be
 * filling bytes and nothing else — no allocation, no callback into unknown
 * code, no early return. A caller that abandons a reservation stops that ring
 * for the life of the process. Bindings that can throw must commit or abort
 * from a finally.
 *
 * `max_bytes` is an UPPER BOUND, not a promise: commit says how much was
 * actually used and the ring advances by that. A generous bound is free
 * except at a wrap, where placement has to be decided before the true length
 * is known and the tail of the ring may go unused.
 *
 * Refusals are the same as send's, and on refusal no lock is held.
 */
uint8_t* clockwork_client_send_begin(ClockworkClient* c, uint32_t max_bytes,
                                     ClockworkStatus* out_status);

/* Publish `bytes` of the open reservation, which must not exceed the bound it
 * was opened with. On CLOCKWORK_E_ARG the reservation is released rather than
 * left open, so a caller that got the length wrong does not also wedge the
 * ring. */
ClockworkStatus clockwork_client_send_commit(ClockworkClient* c, uint32_t bytes,
                                             uint32_t origin);

/* Give back an open reservation without publishing. Nothing a reader can see
 * was changed. Accepted with no reservation open, so an error path can call it
 * unconditionally. */
void clockwork_client_send_abort(ClockworkClient* c);

/* ── Receiving ───────────────────────────────────────────────────────────────
 *
 * One message off the engine's egress. The engine writes two rings — the
 * audio thread's replies, and the NRT gateway's replies and broadcasts —
 * and one poll takes from both; which ring a message rode is not the
 * client's concern. `bytes` points INTO the mapping and is valid until the
 * next poll on this handle — copy it if you need it longer.
 */
typedef struct ClockworkClientMessage {
    const uint8_t* bytes;
    uint32_t       length;
    uint32_t       origin;   /* the token the request carried, or 0 */
    uint32_t       route;    /* CLOCKWORK_ROUTE_* */
    uint32_t       sequence; /* monotonic per ring; a gap means the ring lapped */
} ClockworkClientMessage;

#define CLOCKWORK_ROUTE_REPLY   0u   /* an answer to `origin` */
#define CLOCKWORK_ROUTE_NOTIFY  2u   /* to whoever subscribed to notifications */

/* Take up to `max` messages. Returns how many were written to `out`, which the
 * caller owns and sizes.
 *
 * SINGLE CONSUMER, AND THE RING HAS ONE. Polling TAKES: it advances the
 * egress ring's own read cursor, which lives in the engine's control block and
 * is shared by every handle onto that engine. Two handles polling therefore
 * take from each other — whoever calls first gets the message and the other
 * never sees it. That is what a queue is, and it is what a client draining
 * replies wants.
 *
 * A second reader that wants to WATCH rather than take opens a tap instead;
 * see clockwork_client_tap_open below. Watching is the other shape, and it is
 * a different object precisely because it cannot be had by opening two of
 * these.
 *
 * A client that polls slower than the engine emits will see `sequence` jump.
 * That is the ring lapping, reported rather than hidden, because a client that
 * has missed a reply usually wants to know it missed one.
 */
uint32_t clockwork_client_poll(ClockworkClient* c,
                               ClockworkClientMessage* out, uint32_t max);

/* ── Watching a ring ─────────────────────────────────────────────────────────
 *
 * A TAP reads a ring without taking from it. It carries its own cursor, so
 * several taps and the real consumer all see the same traffic, and none of
 * them shortens anyone else's view.
 *
 * That is what a logger, a traffic inspector or a protocol debugger wants: to
 * show what was sent without becoming part of why it arrived. It is also the
 * only way to see the INGRESS ring at all — the engine is that ring's
 * consumer, and a client that drained it would be eating the engine's input.
 *
 * A TAP CAN BE LAPPED, and that is not an error. Writers reserve space against
 * the real consumer's cursor and know nothing about observers, so a tap that
 * reads too slowly has its unread bytes overwritten underneath it. When that
 * happens the tap resynchronises to the newest data and counts what went past;
 * clockwork_client_tap_missed reports the running total. A caller that cares
 * polls more often or asks for more per call. A caller that does not care is
 * still correct: it simply sees recent traffic rather than all of it.
 *
 * Messages arrive exactly as clockwork_client_poll delivers them, including
 * the route word being peeled from egress frames, so the two read alike and a
 * caller can switch between watching and taking without reshaping its code.
 * `bytes` points into the ring and is valid until the next call on this tap.
 *
 * One tap is for one thread, as one handle is.
 */
typedef struct ClockworkClientTap ClockworkClientTap;

/* Storage for the placement open, as clockwork_client_sizeof is for a handle. */
size_t clockwork_client_tap_sizeof(void);

/* Watch `ring`, which is CLOCKWORK_REGION_INGRESS or CLOCKWORK_REGION_EGRESS.
 * Watching starts at the ring's present head: a tap opened now shows what
 * happens next rather than replaying whatever the ring still holds.
 *
 * The tap borrows the client handle's mapping and must not outlive it. */
ClockworkClientTap* clockwork_client_tap_open(ClockworkClient* c, uint32_t ring,
                                              ClockworkStatus* out_status);

ClockworkClientTap* clockwork_client_tap_open_in(void* storage, size_t storage_bytes,
                                                 ClockworkClient* c, uint32_t ring,
                                                 ClockworkStatus* out_status);

void clockwork_client_tap_close(ClockworkClientTap* t);

uint32_t clockwork_client_tap_poll(ClockworkClientTap* t,
                                   ClockworkClientMessage* out, uint32_t max);

/* Frames written past this tap before it could read them, since it opened. */
uint64_t clockwork_client_tap_missed(const ClockworkClientTap* t);

/* ── Regions ─────────────────────────────────────────────────────────────────
 *
 * The engine publishes several spans of memory, and they are reached by NAME
 * rather than by one accessor each. A region added later is an enumerator, not
 * a new function, so a binding written today keeps compiling and links against
 * a newer library unchanged.
 *
 * ABSENT IS AN ANSWER. An engine with no guest window, or built without the
 * scope, returns CLOCKWORK_E_ABSENT — which a client renders as "nothing to
 * show" rather than treating as an error.
 */
typedef enum ClockworkRegionId {
    CLOCKWORK_REGION_METRICS = 1, /* engine counters; see clockwork_client_metrics */
    CLOCKWORK_REGION_WINDOW,      /* what the GUEST publishes. Opaque here: its
                                   * shape belongs to the guest, and a client
                                   * that parses it does so knowing which guest
                                   * it is talking to. The first uint32 is a
                                   * version stamp the guest bumps — that much
                                   * is contract, and it is all of it. */
    CLOCKWORK_REGION_INBOX,       /* bulk in: the client writes, the guest reads */
    CLOCKWORK_REGION_OUTBOX,      /* bulk out: the guest writes, the client reads */
    CLOCKWORK_REGION_SCOPE,
    CLOCKWORK_REGION_AUDIO_TAPS,
    CLOCKWORK_REGION_INGRESS,     /* the ring clients write and the engine reads */
    CLOCKWORK_REGION_EGRESS,      /* the ring the engine writes and clients read */
    CLOCKWORK_REGION_NATIVE_STATS /* the device layer's own account of its callback:
                                   * DSP load average and peak (centi-percent of the
                                   * callback's budget) and callbacks that overran it.
                                   * A flat uint32 array, described by the nativeStats
                                   * table in metrics_schema.h. Zeros on an engine no
                                   * device drives (attach, a worklet). */
} ClockworkRegionId;

typedef struct ClockworkRegion {
    void*    base;
    uint32_t bytes;
    int      writable;   /* 1 only for the inbox: everything else is the
                          * engine's or the guest's to write */
} ClockworkRegion;

ClockworkStatus clockwork_client_region(ClockworkClient* c, ClockworkRegionId id,
                                        ClockworkRegion* out);

/* ── Metrics ─────────────────────────────────────────────────────────────────
 *
 * The counters are a flat array of uint32, described by a schema the engine
 * ships. Read them as a block: field-by-field reads across a live writer give
 * a set of numbers that never existed together.
 */
uint32_t clockwork_client_metrics(ClockworkClient* c, uint32_t* out, uint32_t max);

/* ── The clock ───────────────────────────────────────────────────────────────
 *
 * One coherent snapshot. bpm and beat origin only mean anything together, so
 * they are read together — a client assembling them from separate calls can
 * catch a tempo change between two of them.
 */
typedef struct ClockworkClientClock {
    uint32_t struct_bytes;

    double   bpm;
    double   beat_origin_ntp;
    double   is_playing_at_ntp;
    int32_t  is_playing;
    uint32_t flags;
    int32_t  meter_num;
    int32_t  meter_den;
} ClockworkClientClock;

ClockworkStatus clockwork_client_clock(ClockworkClient* c, ClockworkClientClock* out);

/* Beat at an NTP instant, under a snapshot the caller already has. Pure
 * arithmetic on the values above — here so that every binding does not
 * re-derive it, and so they all agree to the bit. */
double clockwork_client_beat_at(const ClockworkClientClock* clock, double ntp_seconds);

/* ── Scopes ──────────────────────────────────────────────────────────────────
 *
 * A scope is a ring of audio the engine keeps filling, and reading one is the
 * same three steps wherever it is done: find the slot, ask how far the audible
 * edge has reached, copy a window back from it. Every consumer that has drawn
 * a waveform has written those steps itself against the ring's internals; this
 * is them, once.
 *
 * A slot is a small integer. Which slot carries what is between the engine's
 * embedder and its guest — clockwork numbers them and does not name them.
 */
typedef struct ClockworkScopeReader {
    uint32_t struct_bytes;
    uint32_t slot;
    void*    _internal;   /* the mapped slot; do not touch */
    uint32_t _ring_frames; /* the engine's ring capacity for the slot; do not touch */
} ClockworkScopeReader;

/* Bind a reader to a slot. CLOCKWORK_E_ABSENT when the engine has no such slot
 * — a constrained profile may carry none at all, and a client greys the
 * control rather than drawing zeros. */
ClockworkStatus clockwork_client_scope_open(ClockworkClient* c, uint32_t slot,
                                            ClockworkScopeReader* out);

/* Is the slot still live? A slot deactivates across a device switch and comes
 * back; a reader that keeps drawing through that reads silence rather than
 * stale audio, so a caller re-opens when this turns false. */
int clockwork_client_scope_valid(const ClockworkScopeReader* r);

/* How far the audible edge has reached, in frames since the engine started.
 * Unchanged between calls means nothing new has been rendered — which is the
 * cheap way to skip a redraw. */
uint64_t clockwork_client_scope_audible_end(ClockworkClient* c,
                                            const ClockworkScopeReader* r);

/* Copy `frames` ending at `end` into `out`, interleaved.
 *
 * `out` holds frames * channels floats and is ALWAYS fully written: a slot
 * that went away yields silence rather than whatever the buffer held before,
 * so a caller never draws the previous window's audio by accident.
 *
 * The channel count is re-read from the slot as the copy happens and reported
 * through `out_channels`, because sizing a buffer from an earlier read races a
 * slot that reactivated with a different width.
 */
uint32_t clockwork_client_scope_read(ClockworkClient* c, const ClockworkScopeReader* r,
                                     uint64_t end, uint32_t frames,
                                     float* out, uint32_t* out_channels);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* CLOCKWORK_CLIENT_H */
