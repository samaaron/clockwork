// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * MidiClockOut.h — 24 pulses a beat, timed by ClockworkClock.
 *
 * NOT SCHEDULER CODE. Everything here derives from ClockworkClock: onBeat() subdivides one beat into
 * kPulsesPerBeat absolute NTP times and records them; a follower (below)
 * reads a timeline's grid and records the pulses on it. Every tick knows
 * exactly when it must happen from the moment it is created — there is
 * nothing left to decide, and therefore nothing for a scheduler to decide.
 *
 * NOT A SCHEDULER FEATURE. Routing ticks through clockwork's timed queue would
 * make a build with the queue compiled out have no spread clock at all — and a
 * DSP that is already a scheduler, and so wants no queue, is the last thing
 * that should lose timed output.
 *
 * A tick goes straight to a MIDI sink carrying its own time
 * (clockwork_event_sink.h), and the sink honours that time at the edge: a CoreMIDI
 * packet timestamp, an ALSA queue, Web MIDI's send(bytes, ts). That is TIGHTER
 * than the queue path even where the queue exists, because the timestamp stops
 * being re-derived at each hop and is instead handed to the platform that
 * actually delivers it. Clockwork's own queue is now one possible consumer of
 * outgoing events, not a stage every one of them must pass through.
 *
 * TWO WAYS TO ASK FOR A CLOCK. onBeat() is the client's: one beat's worth of
 * pulses spread over a duration it names, per beat, per request. follow() is
 * the engine's: a continuous 24-PPQN clock on a timeline's own grid — Link's
 * or a midi:<port> follower's — that runs whether or not the transport is,
 * because a MIDI clock always runs and Start (0xFA) / Stop (0xFC) say what to
 * do with it. tick(), called periodically off the audio thread, snapshots
 * each followed timeline and records the pulses due inside a horizon ahead:
 * pulse k at the grid's beat k / 24, the arithmetic in rust/clockwork-clock
 * (midi_clock_out.rs). A follower keeps only the index of the next pulse it
 * has not recorded, so a tempo change re-pins everything still to come with
 * nothing to update, and nothing is ever recorded twice; a transport change
 * records one Start or Stop at the transition the timeline reports.
 *
 * THE HORIZON IS THE PRICE OF PRE-COMMITTING. A pulse in the ring is on the
 * grid as it was when tick() ran; a tempo change reaches the wire only after
 * the pulses already recorded — at most kFollowHorizonSeconds of them, five
 * at 120 BPM. The horizon has to exceed the gap between ticks, with margin,
 * or a late tick starves the port: the producer runs once per audio block on
 * the control pass, and a block is a few tens of milliseconds at most, so a
 * tenth of a second covers a block missed outright. The floor of what is
 * already recorded is kept so a follower that starts, or re-syncs after its
 * grid jumped, begins after it, and a pulse a tempo rise moved behind it is
 * held to it: the ring stays in time order, which generate() relies on.
 *
 * THREADS. onBeat(), follow(), unfollow() and tick() run on the command
 * thread (the control pass's) and are together the one producer; generate()
 * runs on the render thread and is the one consumer. The handoff is a
 * fixed-slot lock-free SPSC ring: neither waits on the other, the audio
 * thread takes no lock and allocates nothing. A full ring drops the tick and
 * counts it. The follower table is command-thread state: fixed capacity,
 * never touched by the render thread. reset() is a flag the consumer honours
 * next call, and forgets every follower at once.
 *
 * WHICH SINK IS RESOLVED ON THE COMMAND THREAD, because opening one allocates
 * and may touch the device — so a slot carries a ClockworkSink, never a port name to
 * be looked up later. The exception is the "every port" burst, which stores
 * CLOCKWORK_SINK_NONE and fans out across the open MIDI sinks at send time; listing
 * them writes into a caller-owned array and allocates nothing, so it is safe
 * where opening one would not be.
 *
 * This file owns *when*. The sink owns *how*.
 */
#pragma once

#include "clockwork_event_sink.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class ClockworkClock;

class MidiClockOut {
public:
    static constexpr int64_t kPulsesPerBeat    = 24;    // 24 PPQN
    // How far ahead of its due time a tick is handed to its sink. The endpoint
    // needs slack to honour a timestamp — a message that arrives at the wire
    // exactly when it is due can only be sent late — and this is that slack.
    static constexpr double  kLookaheadSeconds = 0.01;

    // The MIDI System Real-Time Clock message. One byte, no status/data split,
    // and legal to interleave anywhere — including inside another message.
    static constexpr uint8_t kClockTick = 0xF8;

    // The MIDI System Real-Time Start and Stop messages, sent by a follower
    // on a transport transition. (Continue is not: a timeline reports playing
    // or stopped, not where it resumed from.)
    static constexpr uint8_t kStart = 0xFA;
    static constexpr uint8_t kStop  = 0xFC;

    // Most ports a single "every port" burst fans out to.
    static constexpr uint32_t kMaxFanout = 32;

    // Ports that can follow a timeline at once. Fixed so the table never
    // grows on the command thread while the render thread reads the ring.
    static constexpr uint32_t kMaxFollowers = 8;

    // How far ahead of `now` tick() records a follower's pulses. See the
    // header comment: more than twice the longest gap between ticks, and the
    // longest a tempo change waits behind pulses already recorded.
    static constexpr double kFollowHorizonSeconds = 0.1;

    // Most pulses one follower records per tick. Bounds the batch a tick
    // sorts; at the horizon above it is reached only past 800 BPM, or when
    // catching up after a late tick, when the rest are owed next tick.
    static constexpr uint32_t kMaxPulsesPerTick = 32;

    // Ring depth. One slot is always left empty so a full ring is
    // distinguishable from an empty one, making the usable depth kSlots - 1.
    // Public because the test asserts the exact drop count at the boundary,
    // and a magic 256 in the test would not follow this if it changed.
    static constexpr uint32_t kSlots = 256;   // > 10 simultaneous beats


    // ── Command thread: the one producer ───────────────────────────────
    // One beat = kPulsesPerBeat pulses spread over durationSeconds. `port` is
    // a MIDI port name, or "*" (or empty) for every open MIDI sink.
    void onBeat(ClockworkClock& clock, const std::string& port, double durationSeconds);

    // Follow timeline `timelineId` (a ClockworkClock::timeline id, resolved by the
    // caller from `timeline`, the name the client used) on `port`, sending
    // through `sink` — resolved by the caller, CLOCKWORK_SINK_NONE meaning every
    // open MIDI sink at send time, as for onBeat. A port already following
    // is re-targeted in place and starts afresh on the new grid. False if the
    // table is full. Pulses begin at the next tick().
    bool follow(const std::string& port, const std::string& timeline, int timelineId,
                ClockworkSink sink);
    // Stop following on `port`. Pulses already recorded still go out.
    // False if the port was not following.
    bool unfollow(const std::string& port);

    struct FollowerInfo {
        std::string port;
        std::string timeline;
    };
    // Every follower, in table order.
    std::vector<FollowerInfo> followers() const;

    // The periodic producer step: record every follower's pulses due inside
    // kFollowHorizonSeconds of `nowNtp`, and its Start / Stop if the transport
    // moved. `nowNtp` is the render clock (ClockworkClock::now()), the domain
    // generate() compares against.
    void tick(ClockworkClock& clock, double nowNtp);

    // Drop all pending ticks and forget every follower (engine shutdown /
    // test isolation). NRT only.
    void reset();

    // ── Render thread: the one consumer ────────────────────────────────
    // Send every tick due within the look-ahead window, each carrying its own
    // time. Never allocates, never blocks.
    void generate(double nowNtp);

    // Visibility, not control. `dropped` counts every tick that did not reach
    // a port: a full ring, a sink that refused, and — the easy one to miss —
    // a burst aimed at every port when no MIDI port is open at all.
    uint32_t dropped() const { return mDropped.load(std::memory_order_relaxed); }
    uint64_t sent()    const { return mSent.load(std::memory_order_relaxed); }

private:
    // A tick: when it is due, where it goes, and which byte it is (a clock
    // pulse, or a follower's Start / Stop). CLOCKWORK_SINK_NONE means fan out
    // across every open MIDI sink at send time.
    struct Slot {
        double  atNtp = 0.0;
        ClockworkSink sink  = CLOCKWORK_SINK_NONE;
        uint8_t byte  = kClockTick;
    };
    // Record one tick. False, and counted, if the ring is full.
    bool push(double atNtp, ClockworkSink sink, uint8_t byte);

    // One followed port. Command-thread only.
    struct Follower {
        bool        active     = false;
        std::string port;
        std::string timeline;
        int         timelineId = -1;
        ClockworkSink     sink       = CLOCKWORK_SINK_NONE;
        // The next pulse index to record (CLOCKWORK_MIDI_CLOCK_OUT_UNSTARTED until
        // the first tick).
        int64_t     next       = 0;
        // The transport as last seen: 0 stopped, 1 playing, -1 not yet seen
        // (the first tick adopts it and sends nothing).
        int8_t      playing    = -1;
    };
    Follower* followerFor(const std::string& port);

    Follower              mFollowers[kMaxFollowers];
    // The latest time the follow path has recorded: what a starting follower
    // begins after, and what a pulse is held to, so the ring stays in order.
    double                mFollowFloorNtp = 0.0;

    Slot                  mRing[kSlots];
    std::atomic<uint32_t> mHead{0};       // producer writes, consumer reads
    std::atomic<uint32_t> mTail{0};       // consumer writes, producer reads
    std::atomic<bool>     mResetPending{false};
    std::atomic<uint32_t> mDropped{0};
    std::atomic<uint64_t> mSent{0};
};

// The engine's MidiClockOut, for the render path in audio_processor.cpp —
// which is shared with the worklet build and has no engine object to ask.
// Published by ClockworkEngine at boot beside g_active_clockwork_clock, single
// publisher, cleared at shutdown; null wherever no engine owns one (the
// worklet), and then nothing generates. Not a singleton: the instance is a
// ClockworkEngine member, and a test builds its own.
extern std::atomic<MidiClockOut*> g_active_midi_clock_out;

// Find the open MIDI sink for `port`, opening one if there is none. Command
// thread only. CLOCKWORK_SINK_NONE if `port` is "*" or empty (fan out at send time),
// or if no sink could be opened — a build with no MIDI endpoint refuses, and
// the burst is then generated and dropped rather than silently reshaped.
ClockworkSink midi_clock_sink_for_port(const std::string& port);

// The same lookup with "*" allowed: one sink onto every open output, which is
// what a note to "*" wants (the clock fans out itself because each port's
// ticks are its own). This is THE sink for a port — notes, raw bytes and
// clock ticks all leave through it, so a port's accounting is in one place.
// Command thread only. CLOCKWORK_SINK_NONE if none could be opened.
ClockworkSink midi_sink_for_port(const std::string& port);
