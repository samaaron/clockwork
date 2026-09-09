// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * LinkSession.h — Ableton Link clock-sync session.
 *
 * Owns the cross-machine session timeline: the ableton::LinkAudio instance, its
 * tempo / transport / peers callbacks, the deferred enable/disable worker
 * thread, the peer-name advertisement, and the loopback-only interface filter.
 * The Link Audio bus machinery (sinks / input subs / publish) is NOT here —
 * that is the engine's LinkAudioHost, which borrows the LinkAudio instance
 * this session owns and follows the clock's visibility transitions. The NTP
 * time-source is TimeSource.
 *
 * Session mutators (setBpm / setIsPlaying / requestBeatAtTime / forceBeatAtTime)
 * and Link's own tempo/transport callbacks mirror the converged values back into
 * the ClockworkClockState SAB region that ClockworkClock core owns, so every snapshot
 * reader (including no-Link / WASM) sees the same data shape. The session
 * borrows the owning ClockworkClock& to reach that region via ClockworkClock::state().
 *
 * Two compile shapes selected by CLOCKWORK_LINK:
 *   defined   — LinkSession.cpp: real Ableton session + worker thread.
 *   undefined — the inline session-of-one (this header): thread-free,
 *               Ableton-free. setBpm / transport / beat-origin write straight to
 *               the SAB; isEnabled / getVisibility / isStartStopSyncEnabled read
 *               those flags back; clock RPC answers from the SAB mirror in the
 *               NTP domain; peers return zero/empty.
 *
 * Dispatch is link-time-concrete (no vtable) so the RT-adjacent calls stay
 * branch-free at the call site.
 */
#pragma once

#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#if CLOCKWORK_LINK

#include <memory>
#include <string>

namespace ableton { class LinkAudio; }

class LinkSession {
public:
    // The owning ClockworkClock supplies the SAB mirror (ClockworkClock::state()); the
    // periodic tick is invoked off the RT thread (~every 250 ms) and drives MIDI
    // follower staleness — wired by ClockworkClock so the session never knows MIDI.
    explicit LinkSession(ClockworkClock& clock, std::function<void()> periodicTick);
    ~LinkSession();

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;

    // Spawn the deferred worker. Deliberately NOT started in the constructor: the
    // worker's periodic tick reaches back through the owning ClockworkClock (mClock)
    // into ClockworkClock::mImpl, which is still being assigned by make_unique while
    // this LinkSession — a member of *Impl — is constructing. Starting it there is
    // a data race on mImpl (worker read vs main-thread write, no happens-before).
    // ClockworkClock calls this once from its constructor body, after mImpl is live.
    void startWorker();

    // Anchor the beat grid to wall time when the clock enters service, exactly
    // as the session-of-one below does. Link was TRUSTED to establish the
    // origin through its own callbacks — but those fire only on a change or a
    // peer, so at boot with neither, the grid stayed at origin 0. A grid at
    // origin 0 reads out beats as seconds since 1900 (~4e9), which a client
    // adopts; the FIRST tempo change then re-anchors to Link's own small beat,
    // shifting the domain under that client and throwing every held beat ~4e9
    // seconds off. Placing a real epoch here keeps beats small and consistent
    // from the first read, so a tempo change is continuous. Definition shared
    // in body with the no-Link path; defined in the .cpp, where Impl's clock
    // reference lives.
    void anchorToWallClockIfUnset();

    // Stop and join the deferred worker. Idempotent (a no-op once joined), so
    // the host can call it early in teardown and ~LinkSession's own join then
    // finds nothing to do. Must run before any sibling the worker reaches
    // (MidiTimelines via the periodic tick, the visibility listener via applyVisibility)
    // is torn down.
    void stopWorker();

    using LinkVisibility = ClockworkClock::LinkVisibility;
    using PeerInfo       = ClockworkClock::PeerInfo;

    // ─── Session mutators (mirror into the SAB) ──────────────────────────────
    void setBpm(double bpm);
    void setIsPlaying(bool playing, double atNtpSeconds);
    void setStartStopSyncEnabled(bool enabled);
    void requestBeatAtTime(double beat, double atNtpSeconds, double quantum);
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum);

    // ─── Enable / async enable ───────────────────────────────────────────────
    // applyVisibility wires the deferred worker back to ClockworkClock's visibility
    // orchestrator (the worker may not touch Link Audio sinks directly).
    void setApplyVisibility(std::function<void(LinkVisibility)> apply);
    void requestSetLinkEnabledAsync(bool enabled);
    LinkVisibility lastNonOffVisibility() const;
    void           setLastNonOffVisibility(LinkVisibility v);

    // ─── Visibility primitives (composed by ClockworkClock::setLinkVisibility) ───
    // Tear-down order: Link Audio off, Link off, drop the network-thread
    // priority. Bring-up order: enable (interface filter already set), raise
    // priority, Link Audio on.
    void prepareDisable();
    void enableWithPriority();
    void setLoopbackOnly(bool loopbackOnly);
    bool           isEnabled() const;
    LinkVisibility getVisibility() const;

    // ─── Status ──────────────────────────────────────────────────────────────
    bool   isStartStopSyncEnabled() const;
    size_t numPeers() const;
    std::vector<PeerInfo> listPeers() const;

    // Mirror the live Link clock readouts (peers / tempo / beat / phase /
    // playing) into the dashboard metrics. RT-safe (one lock-free
    // captureAudioSessionState). No-op without Link. Link Audio stream-health is
    // written separately by ClockworkClockNative from the bus bridge.
    void publishLinkClockMetrics(PerformanceMetrics* m, double quantum) const;

    // ─── Peer name ───────────────────────────────────────────────────────────
    void        setPeerName(const char* name);
    const char* peerName() const;

    // ─── Event callbacks (fired on Link's network thread; mirror into SAB) ───
    void setTempoChangedCallback(std::function<void(double)> cb);
    void setNumPeersChangedCallback(std::function<void(std::size_t)> cb);
    // atNtp is when the transport changed, in NTP seconds — the same value
    // written to the clock state, so every reader sees one time.
    void setStartStopChangedCallback(std::function<void(bool playing, double atNtp)> cb);

    // ─── Audio-thread accessors ──────────────────────────────────────────────
    // The borrowed ableton::LinkAudio instance — for LinkAudioHost's bridge and
    // LinkUGen. Cast at the call site keeps this header free of Ableton types
    // where it can be.
    ableton::LinkAudio& linkAudio();
    // link.clock().micros(): Link's per-boot clock, the domain Link Audio
    // aligns streams in. Read only by LinkAudioHost::blockHostMicros.
    int64_t linkClockMicrosRaw() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

#else  // !CLOCKWORK_LINK

// Session-of-one: no Ableton, no thread. Mutators write the SAB mirror directly
// (the same fields the Ableton path converges onto); isEnabled / getVisibility /
// isStartStopSyncEnabled read those flags back; clock RPC answers from the SAB
// mirror; peer reads are zero/empty. Header-inline — no separate TU.
class LinkSession {
public:
    using LinkVisibility = ClockworkClock::LinkVisibility;
    using PeerInfo       = ClockworkClock::PeerInfo;

    explicit LinkSession(ClockworkClock& clock, std::function<void()> /*periodicTick*/)
        : mClock(clock) {}

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;

    // Session-of-one has no worker thread.
    void startWorker() {}
    void stopWorker() {}

    /*
     * Anchor the beat grid and the transport transition to wall time.
     *
     * These initialise to 0 (shared_memory.h): "beat 0 happened at NTP second
     * 0" — midnight, 1st January 1900 — and "no transport transition yet". On
     * the Ableton path Link establishes both through its callbacks and the
     * zeros are never seen. Nothing else did, so on a no-Link build every
     * beat<->time answer came back relative to 1900 (rpc/time_at_beat for beat
     * 8 at 60bpm replied 8000000us — 8 seconds after 1900, and ~4e15us away
     * from the NTP domain the reply is documented to be in), and
     * transport/time replied its 0 sentinel forever.
     *
     * CALLED WHEN THE CLOCK ENTERS SERVICE, NOT WHEN ONE IS CONSTRUCTED. This
     * first ran from startWorker() in ClockworkClock's constructor, which anchored
     * every ClockworkClock ever built — including the bare ones the beat-math unit
     * tests construct, which legitimately expect an origin of 0 so they can
     * assert beatAtTime(0) == 0. Binding into the shared arena is the moment a
     * clock starts answering real clients, and is the moment to give it a real
     * epoch.
     *
     * Skipped if an origin already exists, so a restore or a client that set
     * its own grid is not stamped over.
     */
    void anchorToWallClockIfUnset() {
        ClockworkClockState* s = mClock.state();
        if (!s) return;
        if (clockwork::bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed)) != 0.0)
            return;
        // Beat 0 at now IS the origin — originFor(0, now, bpm) with the beat
        // term dropped, which also avoids 0 * 60 / bpm going NaN before a bpm
        // has been set.
        const double now = wallClockNTP();
        s->setOrigin(now);
        // The transport state itself is unchanged (is_playing stays 0,
        // stopped): what is recorded is WHEN that became true, which is now.
        s->setTransport(false, now);
    }

    // A tempo change must not move the beat that is playing — the rule and
    // its history are with clockwork::retempoOrigin (clock_math.h); the session-of-
    // one was the backing that once forgot it. An unanchored grid stays
    // unanchored: anchorToWallClockIfUnset owns placing it.
    //
    // AND IT MUST BE ANNOUNCED. On the Ableton path Link invokes the tempo
    // callback for a local commit as it does for a peer's, and the engine turns
    // that into /clockwork/clock/notify/tempo — which is how a client that set
    // the tempo through another client learns of it (a language runtime hears a
    // GUI's change this way, and re-anchors its beat on it). This session once
    // dropped the callback on the floor, so a Link-less engine changed tempo
    // in silence and every other client kept the old one.
    void setBpm(double bpm) {
        if (ClockworkClockState* s = mClock.state()) s->retempo(bpm, wallClockNTP());
        if (mTempoCb) mTempoCb(bpm);
    }
    void setIsPlaying(bool playing, double atNtpSeconds) {
        if (ClockworkClockState* s = mClock.state()) s->setTransport(playing, atNtpSeconds);
        if (mStartStopCb) mStartStopCb(playing, atNtpSeconds);
    }
    void setStartStopSyncEnabled(bool enabled) {
        if (ClockworkClockState* s = mClock.state()) s->setFlag(SC_FLAG_START_STOP_SYNC, enabled);
    }
    void requestBeatAtTime(double beat, double atNtpSeconds, double /*quantum*/) {
        ClockworkClockState* s = mClock.state();
        if (!s) return;
        const double bpm = clockwork::bitsToDouble(s->bpm.load(std::memory_order_relaxed));
        s->setOrigin(clockwork::originFor(beat, atNtpSeconds, bpm));
    }
    // No peers in session-of-one — identical to requestBeatAtTime.
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
        requestBeatAtTime(beat, atNtpSeconds, quantum);
    }

    void setApplyVisibility(std::function<void(LinkVisibility)>) {}
    void requestSetLinkEnabledAsync(bool) {}
    LinkVisibility lastNonOffVisibility() const { return LinkVisibility::LoopbackOnly; }
    void           setLastNonOffVisibility(LinkVisibility) {}

    void prepareDisable() {}
    void enableWithPriority() {}
    void setLoopbackOnly(bool) {}
    bool           isEnabled() const {
        const ClockworkClockState* s = mClock.state();
        return s && (s->flags.load(std::memory_order_relaxed) & SC_FLAG_LINK_ENABLED) != 0u;
    }
    LinkVisibility getVisibility() const {
        return isEnabled() ? LinkVisibility::LoopbackOnly : LinkVisibility::Off;
    }

    bool   isStartStopSyncEnabled() const {
        const ClockworkClockState* s = mClock.state();
        return s && (s->flags.load(std::memory_order_relaxed) & SC_FLAG_START_STOP_SYNC) != 0u;
    }
    size_t numPeers() const { return 0; }
    std::vector<PeerInfo> listPeers() const { return {}; }

    void publishLinkClockMetrics(PerformanceMetrics*, double) const {}

    void        setPeerName(const char*) {}
    const char* peerName() const { return ""; }

    void setTempoChangedCallback(std::function<void(double)> cb) { mTempoCb = std::move(cb); }
    // Never fired: a session of one has no peers to count.
    void setNumPeersChangedCallback(std::function<void(std::size_t)>) {}
    void setStartStopChangedCallback(std::function<void(bool, double)> cb) { mStartStopCb = std::move(cb); }

    int64_t linkClockMicrosRaw() const { return 0; }

private:
    ClockworkClock& mClock;
    std::function<void(double)>       mTempoCb;
    std::function<void(bool, double)> mStartStopCb;
};

#endif  // CLOCKWORK_LINK
