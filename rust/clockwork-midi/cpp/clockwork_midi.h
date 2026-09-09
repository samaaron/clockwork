// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_midi.h — C ABI for the clockwork MIDI subsystem (Rust / midir).
 *
 * The C++ engine boundary (src/native/MidiControl) creates one instance, supplying
 * three thread-safe host callbacks, and forwards decoded "/clockwork/midi/" OSC into it via
 * clockwork_midi_handle_osc(). The subsystem owns its midir device IO (and midir's input
 * thread) and never touches the audio thread; results return through the
 * callbacks, which may fire on the midir input thread.
 *
 * Must match rust/clockwork-midi/src/ffi.rs.
 */
#ifndef CLOCKWORK_SS_MIDI_H
#define CLOCKWORK_SS_MIDI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct ClockworkMidi ClockworkMidi;

/* Transport `kind` codes for clockwork_midi_transport_fn. */
#define CLOCKWORK_MIDI_TRANSPORT_START 0
#define CLOCKWORK_MIDI_TRANSPORT_CONTINUE 1
#define CLOCKWORK_MIDI_TRANSPORT_STOP 2
#define CLOCKWORK_MIDI_TRANSPORT_POSITION 3

/* `kind` codes for clockwork_midi_emit_fn. */
#define CLOCKWORK_MIDI_EMIT_BROADCAST 0 /* fan out to the /clockwork/midi/notify audience */
#define CLOCKWORK_MIDI_EMIT_REPLY 1     /* reply to the current caller */

/* Emit an OSC packet to the engine: BROADCAST for "/clockwork/midi/in/" events + the
 * "/clockwork/midi/ports" push, REPLY for "/clockwork/midi/ports.reply". `osc`/`len` are only valid
 * for the duration of the call. */
typedef void (*clockwork_midi_emit_fn)(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

/* One MIDI clock pulse (0xF8) for an input port → ClockworkClock, which anchors the
 * timeline beat on the pulse count (tempo is estimated engine-side).
 * `norm`/`norm_len` is the normalised handle the engine keys the timeline on;
 * `raw`/`raw_len` is the friendly OS device name for display; `ts_us` is the
 * pulse's OS timestamp (µs). Strings not NUL-terminated; valid only during the
 * call. */
typedef void (*clockwork_midi_clock_fn)(void* ctx,
                                 const uint8_t* norm, uint32_t norm_len,
                                 const uint8_t* raw, uint32_t raw_len,
                                 uint64_t ts_us);

/* Transport intent for ClockworkClock, scoped to one input port. `beat` is the
 * target beat for START/POSITION, -1 for CONTINUE/STOP. `norm`/`raw` as above. */
typedef void (*clockwork_midi_transport_fn)(void* ctx,
                                     const uint8_t* norm, uint32_t norm_len,
                                     const uint8_t* raw, uint32_t raw_len,
                                     int32_t kind, double beat);

/* Create the subsystem. `ctx` and the callbacks must outlive it. `app_name`
 * is the client name published to the OS MIDI registry (what other MIDI apps
 * see, e.g. in ALSA/CoreMIDI port lists); NULL or "" falls back to
 * "clockwork". Copied — need not outlive the call. */
ClockworkMidi* clockwork_midi_create(void* ctx,
                       clockwork_midi_emit_fn emit,
                       clockwork_midi_clock_fn clock,
                       clockwork_midi_transport_fn transport,
                       const char* app_name);

/* Stop the clock thread, close all ports, free the instance. */
void clockwork_midi_destroy(ClockworkMidi* handle);

/* Feed one decoded "/clockwork/midi/" OSC packet (off the audio thread). */
void clockwork_midi_handle_osc(ClockworkMidi* handle, const uint8_t* data, uint32_t len);

/* One wire message from clockwork_midi_encode_out: the port ("*" = every open
 * output), the bytes, and the time the verb named with a trailing timetag (0 =
 * none). Strings are not NUL-terminated; pointers are valid during the call. */
typedef void (*clockwork_midi_encode_out_fn)(void* ctx,
                                       const uint8_t* port, uint32_t port_len,
                                       const uint8_t* bytes, uint32_t len,
                                       uint64_t when);

/* Decode one "/clockwork/midi/out/" send verb into its wire bytes without
 * sending them — one callback per message (channel 0 fans out to sixteen).
 * Returns 1 if the packet was a send verb, 0 if it was anything else (hand
 * those to clockwork_midi_handle_osc). The engine uses this to put a note on the
 * sink for its port, which keeps the note's time and the accounting. */
int32_t clockwork_midi_encode_out(const uint8_t* data, uint32_t len,
                            void* ctx, clockwork_midi_encode_out_fn cb);

/* Emit a /clockwork/midi/ports.reply snapshot to the caller (e.g. on new subscription). */
void clockwork_midi_emit_ports(ClockworkMidi* handle);

/* Re-enumerate devices and broadcast the updated /clockwork/midi/ports to subscribers.
 * Called from the engine's hotplug listener on a MIDI device add/remove. */
void clockwork_midi_refresh(ClockworkMidi* handle);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_SS_MIDI_H */
