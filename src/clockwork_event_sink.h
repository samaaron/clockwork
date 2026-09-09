// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_event_sink.h — events leaving clockwork, in one shape.
 *
 * `clockwork_ports.h` did this for frames. This is the other discipline: variable
 * length payloads with a time attached, going out to a MIDI port or an OSC
 * destination.
 *
 * TWO PRODUCERS, WANTING OPPOSITE THINGS
 *
 * A client sends "play this note in one second". It arrives through ingress,
 * must be held, and fires later — the delay is the entire point.
 *
 * A DSP that is itself a scheduler has already decided the moment. When such a
 * DSP emits a note the note is due NOW, and anything between it and the wire is
 * latency added to a decision already correctly made. Its outgoing MIDI never
 * passes through ingress, because it did not come from outside.
 *
 *      client ──ingress──▶ [ scheduler ]──▶ ┐
 *                          (optional)       ├──▶ SINK ──▶ MIDI port / OSC
 *      DSP ─────────────────────────────────┘
 *
 * So clockwork's scheduler is not a service the outputs depend on. It is an
 * optional stage in front of them, used by one producer and bypassed by the
 * other — which is why it can be compiled out without removing a feature.
 *
 * WHEN IS PART OF THE MESSAGE, AND IS HONOURED AT THE EDGE
 *
 * Every send carries `when`. What happens to it is a property of the platform
 * rather than of the sender:
 *
 *   Web MIDI    MIDIOutput.send(bytes, timestamp) — the browser schedules it,
 *               and handing it a future timestamp is TIGHTER than racing to
 *               deliver on time, because worklet-to-main-thread is jittery.
 *   CoreMIDI    MIDIPacket carries a timeStamp; MIDISend schedules on it.
 *   ALSA seq    events schedule on a queue (snd_seq_ev_schedule_real).
 *   WinMM       midiOutShortMsg is immediate; the sink must hold.
 *
 * The closer a timestamp is honoured to the wire, the less jitter it collects
 * crossing threads on the way — so timing belongs at the edge, and a sink that
 * cannot honour it holds the message itself rather than sending it early.
 *
 * THE POINT IS THAT A DSP NEVER BRANCHES ON PLATFORM. It says when it means;
 * the endpoint does the best its platform allows. Without `when` here, every
 * DSP grows a web path and a native path for a decision that is not its
 * business.
 *
 * (Our vendored midir grew a native `send_at` on 2026-08-30: CoreMIDI schedules
 * everywhere but iOS, ALSA where its sequencer gave it a queue, and WinMM has
 * none and refuses, so the sink holds the message itself. This contract was
 * shaped for it, and its arrival changed no DSP and no caller.)
 *
 * NOT FOR ANSWERING CLIENTS. `DspHost::emit_osc` already replies to a request
 * or notifies listeners, carrying an origin token — reply routing a sink has
 * no concept of. Answering a client and sending to a MIDI port look alike and
 * are not the same act; collapsing them would put reply semantics into every
 * endpoint that has no use for them.
 */
#ifndef CLOCKWORK_EVENT_SINK_H
#define CLOCKWORK_EVENT_SINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A slot, stable for the life of the sink. 0 is never valid. */
typedef uint32_t ClockworkSink;
#define CLOCKWORK_SINK_NONE 0u

typedef enum ClockworkSinkKind {
    kClockworkSinkMidi = 1,   /* a MIDI port */
    kClockworkSinkOsc  = 2,   /* an OSC destination (host:port) */
} ClockworkSinkKind;

/* ── Opening ────────────────────────────────────────────────────────────── */

/*
 * Open a sink onto a destination. `target` names it in the endpoint's own
 * terms — a MIDI port name, or "host:port" for OSC.
 *
 * A sink is made of SIZE CLASSES: several queues of different cell widths,
 * and a message takes the narrowest cell it fits. Which classes a kind gets
 * is the host's decision, installed at boot with clockwork_sink_profile below
 * from its memory profile, so a DSP opening a sink gets the right shape for
 * the kind without knowing there is one. `capacity` is the depth asked for
 * the BASE (narrowest) class; it is clamped into the range the implementation
 * allows and rounded up to a power of two, so what you get is at least what
 * you asked for and not necessarily exactly it. 0 keeps the profile's own
 * depth.
 *
 * Control thread only: this allocates and may touch the device.
 */
ClockworkSink clockwork_sink_open(ClockworkSinkKind kind, const char* target, uint32_t capacity);
void    clockwork_sink_close(ClockworkSink sink);

/*
 * The HOST as the MIDI endpoint. On a target with no MIDI port and no thread
 * to drain from — the worklet — a guest's send has nowhere to go but back to
 * the host, which owns the only MIDI there is (Web MIDI, on the main
 * thread). While an emitter is installed, every MIDI sink opened from then on
 * hands each message to it, on the sending thread, with `when` unchanged and
 * the kind and target it was opened with; the host is expected to hold the
 * message to its time (the "Web MIDI" row of the table above), so such a
 * send counts as `scheduled`. Non-zero back means taken; zero is a drop.
 *
 * The engine installs its own (a ring write onto the egress, safe on the
 * audio thread) where the host is the far end of clockwork's chain — see
 * clockwork_host_forward in lanes.h. NULL uninstalls. Sinks already open
 * keep the endpoint they have.
 */
typedef int (*ClockworkSinkHostEmit)(uint32_t kind, const char* target,
                                     const uint8_t* bytes, uint32_t len, int64_t when);
void clockwork_sink_set_host_emit(ClockworkSinkHostEmit emit);

/*
 * Install the size classes every sink of `kind` opened from now on is made
 * of: `n` classes, `cell_bytes[i]` wide and `cells[i]` deep; order does not
 * matter, depths round up to a power of two. Returns non-zero, or zero for a
 * shape that does not hold together (nothing, or one width named twice), in
 * which case the previous shape stays. What a class costs is
 * cells × cell_bytes of payload, reserved at every open.
 *
 * The host's, at boot, from memory_profile.h. A DSP does not call this.
 */
int clockwork_sink_profile(ClockworkSinkKind kind, uint32_t n,
                           const uint32_t* cell_bytes, const uint32_t* cells);

/* The widest message a sink can carry, and the payload bytes it reserved at
 * open. 0 for a handle that is not live. */
uint32_t clockwork_sink_max_message_bytes(ClockworkSink sink);
uint64_t clockwork_sink_bytes_reserved(ClockworkSink sink);

/*
 * A handle that is closed, stale, or was never valid answers the empty thing
 * — 0, 0 and NULL — rather than faulting. Same rule clockwork_ports.h states:
 * the audio thread is not the place to discover a lifetime bug.
 *
 * `clockwork_sink_target` hands out the sink's own copy of the string, so it is
 * valid exactly as long as the sink is. Diagnostics, not something to hold.
 */
int         clockwork_sink_is_open(ClockworkSink sink);
ClockworkSinkKind clockwork_sink_kind(ClockworkSink sink);
const char* clockwork_sink_target(ClockworkSink sink);

/* ── Sending ────────────────────────────────────────────────────────────── */

/*
 * Send one message, to be delivered at `when` — an OSC timetag in the same
 * domain as dsp_process's block_time, with 1 meaning immediately. The same
 * time domain across the whole boundary; conversion to a platform's own clock
 * happens once, in the endpoint.
 *
 * Returns non-zero if accepted. A full sink DROPS and counts rather than
 * blocking: an output that stalls the audio thread is worse than an output
 * that misses a note and says so.
 *
 * Callable from the audio thread. Never allocates, never locks, never blocks.
 *
 * `when` IS SIGNED HERE AND UNSIGNED IN MEANING. An OSC
 * timetag is 32.32 fixed point NTP seconds since 1900, and 2026 is more than
 * 2^31 seconds after 1900 — so the top bit of every REAL timetag is already
 * set, and every real timetag is a NEGATIVE int64_t. Compare two of them
 * signed and "immediately" (1, a small positive) sorts after next Tuesday.
 * The type matches dsp_process's block_time and stays as it is; every
 * comparison of one is done as uint64_t.
 *
 * A MESSAGE WIDER THAN THE WIDEST CLASS IS REFUSED AND COUNTED AS A DROP. A
 * sink's memory is taken at open — that is what makes this call
 * allocation-free — so every cell has a fixed width and a message wider than
 * the widest cannot be carried. It is refused whole and counted `dropped`,
 * never truncated: a shortened MIDI message is a DIFFERENT message and no
 * reader could tell. clockwork_sink_max_message_bytes says where the line is.
 * A message that fits a class whose cells are all taken tries the next wider
 * class before it is refused.
 */
int clockwork_sink_send(ClockworkSink sink, const uint8_t* bytes, uint32_t len, int64_t when);

/* ── Health ─────────────────────────────────────────────────────────────── */

/*
 * What went out and what did not.
 *
 * `late` is the one worth watching: messages the endpoint could not honour the
 * time of, because the platform had no timestamped send and the message had
 * already come due by the time it was drained. It is the number that says
 * whether timing is actually working — and it exists because a DSP holding its
 * own schedule leaves clockwork unable to see when anything was MEANT to
 * happen. Without it, a complaint about timing is unfalsifiable.
 */
typedef struct ClockworkSinkStats {
    uint64_t sent;
    uint64_t dropped;    /* the sink was full */
    uint64_t late;       /* delivered, but after its time */
    uint64_t scheduled;  /* handed to the platform to deliver later */
    uint64_t cancelled;  /* a flush took them before they went out */
} ClockworkSinkStats;

/*
 * HOW THE FIVE ADD UP — the comments above name each number, not the
 * arithmetic between them:
 *
 *   sent      — handed to the endpoint, whatever the endpoint then did.
 *   scheduled — the SUBSET of `sent` given to a platform that will deliver it
 *               later itself. Zero wherever no such platform exists.
 *   late      — the SUBSET of `sent` that was already past due when the drain
 *               first saw it.
 *   dropped   — never handed over: the sink was full, the message was too
 *               long for a cell, or the endpoint refused it.
 *   cancelled — accepted, and then taken by a flush before it went out.
 *
 * So `sent + dropped + cancelled` accounts for every message the sink was
 * offered, and `scheduled` and `late` are views INTO `sent` rather than
 * additions to it.
 *
 * AND `late` IS COUNTED AT FIRST SIGHT, not at delivery. Read literally, "the
 * message had already come due by the time it was drained" counts everything:
 * a sink that holds a message until its moment necessarily releases it a hair
 * after that moment. So the test is applied when the drain FIRST SEES a
 * message — if it was already overdue then, the sink never had the chance to
 * honour it. A sink that is working reports zero, which is what makes the
 * number worth watching at all.
 */
int clockwork_sink_stats(ClockworkSink sink, ClockworkSinkStats* out);

/* ── Cancelling ─────────────────────────────────────────────────────────── */

/*
 * Cancel everything a sink is still holding. Returns how many went.
 *
 * ONLY what is still here. A message already handed to the platform cannot be
 * recalled — that is the price of letting the kernel or the MIDI server do the
 * fine timing — but the exposure is BOUNDED: a sink hands over only what falls
 * inside a short look-ahead, so at most that window is uncancellable. Beyond
 * it, everything is still reachable.
 *
 * THIS CANCELS AND NOTHING ELSE. It emits no all-notes-off and makes no
 * decision about what a caller meant; a note already sounding goes on
 * sounding. Cancelling pending messages and silencing a device are two acts,
 * and the second needs knowledge clockwork does not have — which ports,
 * which channels. Whoever defines what "stop" means composes them.
 *
 * Callable from a control thread. flush_all is what a run-stop reaches for:
 * one call covers every port and destination, whoever queued the message —
 * a guest that sent its own through clockwork_sink_send is covered by the same call
 * as a client that sent verbs. That is the point of putting cancellation on
 * the sink rather than on whichever queue a message happened to pass through.
 */
uint32_t clockwork_sink_flush(ClockworkSink sink);
uint32_t clockwork_sink_flush_all(void);

/* Every open sink, for metrics and for a debug log a user submits. */
uint32_t clockwork_sink_list(ClockworkSink* out, uint32_t cap);

/*
 * Close every open sink: shutdown wants one call rather than a list-and-loop,
 * and one test case must not leak slots into the next. Not for the audio
 * thread.
 *
 * A CLOSE DISCARDS WHAT THE SINK WAS STILL HOLDING — messages accepted, not
 * yet due, and therefore not yet sent. Flushing them would send them early,
 * which is the one thing this file says a sink must never do. The cost is
 * real and is named here rather than discovered: a note-off scheduled ahead
 * of a close does not go out, so a producer that closes a sink with notes in
 * flight should send its own all-notes-off first.
 */
void clockwork_sink_close_all(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_EVENT_SINK_H */
