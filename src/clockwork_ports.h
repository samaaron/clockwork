// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_ports.h — audio crossing the boundary, in one shape.
 *
 * Clockwork moves audio between the audio thread and something that is not
 * the audio thread in seven different places, and every one grew its own
 * plumbing: a threaded file writer, two shared-memory rings with different
 * protocols, Link's aux publish, Link's peer input, and the device in both
 * directions. An eighth — streaming a sample off disk — was wanted and never
 * built, and would have been a ninth invention.
 *
 * They are one object. A PORT is:
 *
 *   a SLOT        — stable for the lifetime of the stream in it. When a
 *                   stream ends its slot goes silent and stays reserved.
 *                   Renumbering would hand a reader somebody else's audio on
 *                   a channel it is already processing, which is a fault that
 *                   presents as a mystery rather than an error.
 *   a DIRECTION   — into clockwork (a source) or out of it (a sink).
 *   CHANNELS      — fixed for the life of the port.
 *   a RING        — single producer, single consumer, lock-free.
 *   an ENDPOINT   — whoever fills a source or drains a sink, off the audio
 *                   thread. The substrate does not know what an endpoint is.
 *   a POLICY      — a source that has nothing ready reads as SILENCE, never
 *                   as stale; a sink with no room DROPS. Both are counted and
 *                   both are published. Neither ever blocks the audio thread.
 *
 * WHAT MAKES IT GENERIC is that last point: no code here knows what an
 * endpoint is. Disk, Link and the network appear below only as examples of
 * one. A disk reader and a Link peer are both endpoints calling
 * clockwork_port_produce; a file writer and Link's aux publish are both endpoints
 * calling clockwork_port_consume. Adding an endpoint — another process, a
 * plugin, a scope, a capture buffer — adds nothing here.
 *
 * THE AUDIO THREAD MOVES FRAMES ONLY THROUGH clockwork_port_read / clockwork_port_write.
 * Those two never allocate, never lock, and never block; they answer
 * immediately with however many frames were really there, and count the
 * shortfall. (The query calls — clockwork_port_channels, clockwork_port_direction — are
 * allocation-free and lock-free too; a caller routing ports to channels reads
 * them per block.)
 *
 * Frames only. Events (OSC, MIDI, gamepad) are the other discipline and have
 * their own rings already; the two are deliberately not one type, because
 * frames want a fixed cadence and silence-on-underrun while events want
 * variable size and a counted drop. See docs/PORTS.md.
 */
#ifndef CLOCKWORK_PORTS_H
#define CLOCKWORK_PORTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A slot. 0 is never a valid port, so it doubles as "none". */
typedef uint32_t ClockworkPort;
#define CLOCKWORK_PORT_NONE 0u

typedef enum ClockworkPortDirection {
    kClockworkPortSource = 1,   /* endpoint -> clockwork (disk playback, Link peer, …) */
    kClockworkPortSink   = 2,   /* clockwork -> endpoint (recording, Link publish, …) */
} ClockworkPortDirection;

/* ── Opening and closing ────────────────────────────────────────────────── */

/*
 * Open a port and get its slot, or CLOCKWORK_PORT_NONE if the registry is full or
 * the arguments make no sense.
 *
 * `name` is for diagnostics and for a client to recognise its own stream; it
 * is copied. `capacity_frames` is the ring's depth per channel and is the
 * whole latency/robustness trade: deep enough to ride out a slow disk read or
 * a late network packet, shallow enough not to add latency nobody asked for.
 * It is rounded up to a power of two, and clamped into the range the ring
 * allows.
 *
 * Call from a control thread, never the audio thread: this allocates.
 */
ClockworkPort clockwork_port_open(const char* name, ClockworkPortDirection dir,
                      uint32_t channels, uint32_t capacity_frames);

/*
 * Close a port. The slot goes silent immediately and is NOT reused until
 * every reader has been told — reads answer silence and writes are discarded
 * in the meantime. Safe to call while the audio thread is running; safe to
 * call twice.
 */
void clockwork_port_close(ClockworkPort port);

/* Shape, for a caller that has only the slot. 0 if the port is closed. */
uint32_t clockwork_port_channels(ClockworkPort port);
int      clockwork_port_is_open(ClockworkPort port);

/*
 * Which way the port runs, or 0 if it is closed.
 *
 * clockwork_port_list hands back sources and sinks mixed together, and a caller
 * that means to read the sources cannot otherwise tell which it may read.
 * Without this each consumer keeps a shadow record of a fact the substrate
 * already holds, which is the duplication ports exist to end.
 */
ClockworkPortDirection clockwork_port_direction(ClockworkPort port);

/* ── The audio thread ───────────────────────────────────────────────────── */

/*
 * Read up to `frames` from a source into `out` (one pointer per channel).
 * Returns how many frames were really available; the remainder of `out` is
 * ZEROED, so a caller may always use the whole block. An endpoint that has
 * fallen behind therefore sounds like silence rather than like a glitch, and
 * the shortfall is counted.
 *
 * `channels` is the caller's count. If the port has fewer, the extra output
 * channels are zeroed; if it has more, the extras are discarded. A count
 * mismatch is a fact to handle, not an error to refuse — the device can
 * change underneath a running stream.
 */
uint32_t clockwork_port_read(ClockworkPort port, float* const* out,
                       uint32_t channels, uint32_t frames);

/*
 * Write `frames` from `in` into a sink. Returns how many were accepted;
 * anything not accepted is dropped and counted. Never blocks, so a slow
 * endpoint costs audio nothing but its own recording.
 */
uint32_t clockwork_port_write(ClockworkPort port, const float* const* in,
                        uint32_t channels, uint32_t frames);

/* ── The endpoint's side ────────────────────────────────────────────────── */

/*
 * Fill a source (produce) or drain a sink (consume), INTERLEAVED, from an
 * ordinary thread. Both return the frame count actually transferred: a
 * producer that gets less than it offered should keep the remainder and try
 * again rather than drop it, because the ring being full means the audio
 * thread is simply not there yet.
 *
 * Lock-free, so these may be called from a realtime-ish thread too, but they
 * are meant for the disk reader, the file writer, the network peer.
 */
uint32_t clockwork_port_produce(ClockworkPort port, const float* interleaved, uint32_t frames);
uint32_t clockwork_port_consume(ClockworkPort port, float* interleaved, uint32_t frames);

/* How many frames could be produced into / consumed from this port right now.
   A disk reader uses this to decide whether to bother reading. */
uint32_t clockwork_port_writable(ClockworkPort port);
uint32_t clockwork_port_readable(ClockworkPort port);

/* ── Health ─────────────────────────────────────────────────────────────── */

/*
 * Frames the audio thread wanted and did not get (source), and frames the
 * audio thread offered a sink that would not fit because the endpoint had
 * not drained it. These are the numbers that say whether a stream is actually
 * working, and the reason both are counted rather than merely handled:
 * silence is a correct response to an underrun and an indistinguishable one
 * from silence that was meant.
 */
uint64_t clockwork_port_underruns(ClockworkPort port);
uint64_t clockwork_port_overruns(ClockworkPort port);

/* Every open port's slot, for metrics and for a client asking what exists.
   Writes at most `cap` and returns how many there are. */
uint32_t clockwork_port_list(ClockworkPort* out, uint32_t cap);

/* Diagnostics: the name given at open, or NULL. Valid until the port closes. */
const char* clockwork_port_name(ClockworkPort port);

/*
 * Close every open port: for shutdown, and so one test case cannot leak slots
 * into the next. Not for the audio thread.
 */
void clockwork_port_close_all(void);

/* ── Shared storage, and a transfer signal ──────────────────────────────── */

/*
 * A port whose endpoint is ANOTHER PROCESS cannot keep its ring in this
 * process's heap: the frames must live
 * where both can see them. These two calls make that a port like any other.
 *
 * clockwork_port_open_shared opens a port over memory the caller owns —
 * clockwork_port_shared_bytes(channels, capacity) bytes, 64-byte aligned, mapped
 * for as long as the port is open. The ring's two counters live in the
 * first CLOCKWORK_PORT_SHARED_HEADER_BYTES of it, so the far side, which lays out
 * the same memory by hand or opens its own port over it, agrees on the fill
 * without any other channel between them. The side that CREATES the memory
 * passes reset=1, once, before anyone else can see it; a side that JOINS
 * memory already in use passes 0, or it would throw away what is in flight.
 *
 * The geometry is exact rather than rounded: a capacity that is not a power
 * of two, or storage smaller than the shape needs, is refused with 0, because
 * the far side sized the same memory by the same numbers.
 *
 * THE FAR SIDE IS NOT TRUSTED. It may be dead, wedged or hostile, and its
 * counters are in memory it can write. The ring clamps every count and masks
 * every index, so a nonsense counter costs frames — a drop, a block of stale
 * samples — and never a memory fault here. What it cannot do is make the
 * far side keep up: that is the signal's job.
 */
#define CLOCKWORK_PORT_SHARED_HEADER_BYTES 128

ClockworkPort clockwork_port_open_shared(const char* name, ClockworkPortDirection dir,
                             uint32_t channels, uint32_t capacity_frames,
                             uint8_t* storage, size_t storage_bytes, int reset);

/* Bytes the shape needs, header included; 0 for a shape a port cannot be. */
size_t clockwork_port_shared_bytes(uint32_t channels, uint32_t capacity_frames);

/*
 * The TRANSFER SIGNAL: `f(port, ctx)` is called on the audio thread after each
 * clockwork_port_write on a sink, or clockwork_port_read on a source — that is, once per
 * block, at the moment there are new frames to take or new room to fill.
 * It exists so that an endpoint in another process can be WOKEN rather than
 * left polling: the bridge's audio thread sleeps on a semaphore the signal
 * posts. Whatever `f` does it does on the audio thread, so it must be what a
 * semaphore post is — a few instructions and no lock. A NULL `f` removes the
 * signal. Returns 0 if the port is not open.
 */
typedef void (*ClockworkPortSignal)(ClockworkPort port, void* ctx);
int clockwork_port_set_signal(ClockworkPort port, ClockworkPortSignal f, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_PORTS_H */
