// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * MidiControl.h — the "/clockwork/midi/" engine boundary.
 *
 * Owns the Rust midir-based MIDI subsystem (clockwork_midi_*, first-party crate) and
 * bridges it to the engine: forwards "/clockwork/midi/" control OSC into it, and routes
 * its callbacks back out — "/clockwork/midi/in/" events + "/clockwork/midi/ports" to the egress hub,
 * clock-in pulses and transport to ClockworkClock. Subscription manages the egress
 * audience.
 */
#pragma once

#include <cstdint>

struct ClockworkMidi;       // rust/clockwork-midi/cpp/clockwork_midi.h
class OscEgress;
class ClockworkClock;
class MidiClockOut;
struct DrainCallCtx;

class MidiControl {
public:
    // `clockOut` is the engine's MidiClockOut: this is its one producer
    // (the control pass's thread), the render thread its one consumer.
    void init(OscEgress* egress, ClockworkClock* clock, MidiClockOut* clockOut);
    void shutdown();

    // Handle one "/clockwork/midi/" command off the audio thread (NRT gateway). Returns
    // true if it belongs to this subsystem (always, for a "/clockwork/midi/" prefix).
    // Reached by both immediate "/clockwork/midi/" traffic and scheduled "/clockwork/midi/" events
    // re-ingested on time (the scheduler feeds the same dispatch).
    bool handleMidiCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

    // Re-enumerate MIDI devices and broadcast /clockwork/midi/ports — called from the
    // engine's device-change (hotplug) listener.
    void refreshDevices();

private:
    // clockwork_midi_* host callbacks (ctx = this). clock/transport carry the
    // normalised port handle + raw OS name (length-delimited, not NUL-term).
    static void    emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);
    static void    clockCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                           const uint8_t* raw, uint32_t rawLen, uint64_t tsUs);
    static void    transportCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                               const uint8_t* raw, uint32_t rawLen, int32_t kind, double beat);
    // Broadcast a /clockwork/clock/timelines push when the timeline set changes.
    void           broadcastTimelines();
    // Route the clock-out verbs to MidiClockOut: /clockwork/midi/clock/beat (a
    // client's midi_clock_beat) and the follower verbs follow / unfollow /
    // followers. Returns false if the message is none of them, or if there
    // is no clock to time against.
    // Runs on the control pass's thread (immediate and scheduled traffic alike),
    // which is MidiClockOut's one producer.
    bool           handleClockOutVerb(const uint8_t* data, uint32_t size);
#if CLOCKWORK_CLIENT_VERBS
    // "/clockwork/midi/out/*" sends: onto the sink for the port, with the
    // message's time. False for any other verb. The client half of the
    // surface (docs/SURFACE.md): not compiled when the build has none.
    bool           handleOutVerb(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);
#endif

    ClockworkMidi*      mMidi     = nullptr;
    OscEgress*    mEgress   = nullptr;
    ClockworkClock*     mClock    = nullptr;
    MidiClockOut* mClockOut = nullptr;
    // The current command's origin token, held for the duration of a synchronous
    // clockwork_midi_handle_osc call so emitCb (a Rust callback with no call ctx) can
    // route its REPLY back to the caller. NRT-thread-only; REPLY emits are
    // synchronous within handleMidiCommand. Async emits (/clockwork/midi/in, ports) broadcast.
    uint32_t    mReplyToken = 0;
};
