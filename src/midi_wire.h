// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * midi_wire.h — a short MIDI message to and from its "/clockwork/midi/…" OSC
 * form, with no allocation (rust/clockwork-midi-wire).
 *
 * What the audio thread converts with when its host supplies the MIDI ports
 * (lanes.h, clockwork_host_midi_ports): a DAW's events in, a client's sends
 * out. The forms are the device subsystem's own, byte for byte. Short
 * messages only: system exclusive has no bound, so it is not carried.
 *
 * Every call is audio-thread safe: no allocation, no lock, no wait.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The longest /clockwork/midi/in/… message a short message becomes, for a
 * port name of at most CLOCKWORK_MIDI_WIRE_PORT_MAX bytes. */
#define CLOCKWORK_MIDI_WIRE_PORT_MAX 63u
#define CLOCKWORK_MIDI_WIRE_IN_MAX   144u

/* midi[0..len), one whole channel message from `port`, as
 * "/clockwork/midi/in/<kind> <port> <channel> <values…> [<when>]" in
 * out[0..cap): the bytes written, 0 for nothing converted (not one channel
 * message, no room). `when` is the time the bytes arrived, an OSC timetag,
 * sent as a trailing `t`; 0 leaves it off. */
uint32_t clockwork_midi_wire_in(const char* port, const uint8_t* midi, uint32_t len, uint64_t when,
                                uint8_t* out, uint32_t cap);

/* A send read from "/clockwork/midi/out/<kind> <port> <channel> <values…>
 * [<when>]". `port` points into the message read, NUL-terminated there, and
 * is valid as long as the message is. With every_channel the channel nibble
 * of bytes[0] is 0 and the message is for each of the 16. `when` is the
 * message's own trailing timetag, 0 for none. */
typedef struct ClockworkMidiWireOut {
    const char* port;
    uint64_t    when;
    uint8_t     bytes[3];
    uint8_t     len;
    uint32_t    every_channel;
} ClockworkMidiWireOut;

/* 1 with *send filled; 0, and *send untouched, for anything that is not a
 * short send: another verb (a port's enable), system exclusive, arguments
 * that cannot be read. */
int clockwork_midi_wire_out(const uint8_t* osc, uint32_t len, ClockworkMidiWireOut* send);

/* A host's ports as the port list: "/clockwork/midi/ports.reply" (`reply`
 * non-zero, an answer) or "/clockwork/midi/ports" (a notice), "<nIn> [name
 * enabled]* <nOut> [name enabled]*". NULL or "" is no port. The bytes
 * written to out[0..cap), 0 for no room. */
uint32_t clockwork_midi_wire_ports(const char* input, const char* output, int reply, uint8_t* out, uint32_t cap);

#ifdef __cplusplus
}
#endif
