// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * ClockworkClock.h — engine session-timeline service.
 *
 * Owns the engine's view of the Link session: tempo, transport, beat
 * origin, peers, and audio-thread-derived NTP. Native wraps an Ableton Link
 * session (real cross-machine sync) when CLOCKWORK_LINK is set; otherwise falls
 * back to the local ClockworkClockState atomics. WASM always uses the local-state
 * path (no UDP in the browser).
 *
 * Every time this class takes or gives is NTP seconds. A reader wanting
 * beat math asks for timeline(id) — one coherent copy of a grid — and does
 * the arithmetic on the copy. Link's own clock is not spoken here at all:
 * the audio half of Link (sinks, peer channels, the block stamp in Link's
 * domain) is LinkAudioHost, which borrows this clock's session and follows
 * its visibility transitions as a LinkVisibilityListener.
 *
 * Same public API on both builds — callers don't branch on platform.
 */
#pragma once

#include "clock/Timeline.h"
#include "shared_memory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LinkSession;

// ─── ClockworkClock ──────────────────────────────────────────────────────────────
class ClockworkClock {
public:
    ClockworkClock();
    ~ClockworkClock();

    ClockworkClock(const ClockworkClock&) = delete;
    ClockworkClock& operator=(const ClockworkClock&) = delete;
    ClockworkClock(ClockworkClock&&) = delete;
    ClockworkClock& operator=(ClockworkClock&&) = delete;

    // ─── Teardown (app-thread) ────────────────────────────────────────────
    // Stop and join the session's background worker (the ~250 ms MIDI-staleness
    // tick / async Link enable). Idempotent. The host MUST call this before
    // tearing down anything the worker reaches — the SHM arena the clock state
    // is bound into, the Link Audio bus — because the worker drives MIDI
    // staleness through the clock and would otherwise run against freed state in
    // the window before ~ClockworkClock joins it. No-op on thread-free builds.
    void stopBackgroundWork();

    // ─── Session mutators (app-thread) ────────────────────────────────────

    void setBpm(double bpm);
    void setIsPlaying(bool playing, double atNtpSeconds);
    // The Link timeline's meter: how quarter-note beats group into bars
    // (timeline(0).meter_num / meter_den, and its barAt). Written straight
    // to the clock state on every build — Link carries no meter, so the
    // session is not involved. False, and nothing written, for a meter
    // clockwork_timeline_meter_valid refuses.
    bool setMeter(int num, int den);
    void setLinkEnabled(bool enabled);

    // Audio-thread-safe variant of setLinkEnabled; defers the
    // blocking work to a ClockworkClock-owned worker thread.
    void requestSetLinkEnabledAsync(bool enabled);
    void setStartStopSyncEnabled(bool enabled);
    void requestBeatAtTime(double beat, double atNtpSeconds, double quantum);
    void forceBeatAtTime(double beat, double atNtpSeconds, double quantum);

    // ─── Session-state getters (app-thread) ───────────────────────────────

    double getBpm() const;
    bool   isPlaying() const;
    double getBeatOriginNtp() const;
    double getIsPlayingAtNtp() const;
    bool   isLinkEnabled() const;
    bool   isStartStopSyncEnabled() const;
    size_t numPeers() const;

    // ─── Beat math (app-thread) ───────────────────────────────────────────
    // Pure functions of (bpm, beat_origin). Reads each field independently
    // — for a coherent multi-field read use readClockworkClock(state()).

    double beatAtTime(double ntpSeconds, double quantum) const;
    double phaseAtTime(double ntpSeconds, double quantum) const;
    double timeAtBeat(double beat, double quantum) const;

    // ─── The timeline as a value (any thread) ─────────────────────────────
    // One coherent copy of a beat grid, in NTP seconds: id 0 is the Link
    // timeline read from the clock state, 1..K a midi follower slot, and any
    // id nothing holds is the 60 BPM placeholder (so every question still has
    // an answer). The /clockwork/clock verbs answer from this and nothing else.

    clockwork::Timeline timeline(int id) const;

    // The same for a midi slot, WITHOUT THE REGISTRY LOCK: the audio
    // thread's reader (the engine mirrors the follower timelines into the
    // plugin bridge's segment once per block). An unheld slot reads as the
    // id -1 placeholder. False, `out` untouched, for id 0, an id past the
    // registry, or a writer caught mid-way; a caller keeps its last snapshot
    // then. Everything else on this class that reaches the registry takes
    // its lock and stays off the audio thread.
    bool timelineRt(int id, clockwork::Timeline& out) const;

    // ─── MIDI-clock follower timelines ────────────────────────────────────
    // Compiled on every build; fed by the MIDI subsystem on native, or by the
    // manual /clockwork/clock/midi:<port>/ OSC set-verbs elsewhere.
    // ClockworkClock owns a fixed registry of midi:<port> follower timelines,
    // separate from the Link timeline. The MIDI subsystem feeds tempo /
    // transport per port; OSC clients read them via /clockwork/clock/midi:<port>/*.
    // Slot assignment, primary selection, and staleness all live here.
    //
    // Timeline id: 0 = Link (routes to the getters above); 1..K = midi
    // slots. The registry itself is Rust (rust/clockwork-clock) behind
    // MidiTimelines; every time it takes or gives is NTP seconds.

    // Find-or-allocate the slot for a port; idempotent. `normalized` is the
    // OSC-safe handle (match key + /clockwork/clock/midi:<handle>/ address segment);
    // `raw` is the original OS device name, kept for display. Returns 1..K,
    // or -1 if the registry is full. Fed off the RT thread (MIDI subsystem).
    int  claimMidiTimeline(const char* normalized, const char* raw);
    void freeMidiTimeline(int id);

    // Resolve a /clockwork/clock/<tl>/ name to an id: "" / "link" → 0; "midi" (bare) →
    // the primary midi slot; "midi:<port>" → that port's slot. Returns -1 for an
    // unclaimed port or a malformed name (read methods treat id -1 as a 60 BPM
    // placeholder; resolve never auto-claims — claims happen on the feed path).
    int  resolveTimeline(const char* name) const;

    // Write-path variant: resolves like the above, but a "midi:<port>" name
    // claims the slot if it hasn't clocked yet (so the OSC manual-set/transport
    // path doesn't special-case the midi: grammar). Bare "midi" can't claim.
    int  resolveOrClaimTimeline(const char* name);

    // Live clock feed: one 0xF8 pulse at OS timestamp `tsUs`. The beat is the
    // exact pulse count; the tempo is smoothed separately for interpolation.
    void midiTimelinePulse(int id, uint64_t tsUs);
    // Manual tempo set (OSC / unfed placeholder): advances beat continuously;
    // a live clock's pulses override it.
    void setMidiTimelineTempo(int id, double bpm);
    void setMidiTimelineTransport(int id, int kind, double beat);
    // A midi timeline's meter (no MIDI message carries one). False for an
    // invalid meter or an unheld slot.
    bool setMidiTimelineMeter(int id, int num, int den);

    // Staleness sweep (called periodically off the RT thread): marks stale
    // after a feed gap, freezing the tempo so the timeline free-runs. Stale
    // slots are not freed — they are reclaimed (oldest-stale first) only when
    // a new port needs one.
    void tickMidiStaleness();

    // Single-field conveniences over timeline(id).
    double  timelineBpm(int id) const       { return timeline(id).bpm; }
    bool    timelineIsPlaying(int id) const { return timeline(id).playing != 0; }
    // Whether a transport event (START or SPP) has defined the timeline's beat
    // origin. Without one, beats are arbitrary 24-pulse groupings from the
    // first pulse seen, so bar phase is meaningless. Link (id 0) is always
    // anchored — its session grid exists independent of transport.
    bool    timelineIsAnchored(int id) const { return timeline(id).anchored != 0; }

    // Enumeration snapshot for /clockwork/clock/timelines/get.
    struct TimelineInfo {
        std::string name;            // wire identity: "link" | "midi:<handle>"
        std::string raw;             // original OS device name (display); "link" for Link
        double      bpm{0.0};
        bool        clocking{false};
        bool        stale{false};
        bool        primary{false};
    };
    std::vector<TimelineInfo> listTimelines() const;

    // Fired (off RT) when the timeline set changes — add / remove / stale /
    // primary. At most one callback; setting replaces. Never fires on WASM.
    void setTimelinesChangedCallback(std::function<void()> cb);

    // ─── Link event callbacks (registered by app code; fired on Link's
    // network thread). At most one callback per kind; setting replaces.

    void setTempoChangedCallback(std::function<void(double bpm)> cb);
    void setNumPeersChangedCallback(std::function<void(std::size_t)> cb);
    void setStartStopChangedCallback(std::function<void(bool playing, double atNtp)> cb);

    // ─── Link: visibility + peer name ────────────────────────────────────

    // Three-state network visibility for the Link session (and whatever
    // rides on it — see LinkVisibilityListener):
    //   Off          — no peer discovery, no audio sharing
    //   LoopbackOnly — discovery + audio sharing on lo0 only (same-machine
    //                  peers can find us; LAN cannot)
    //   NetworkWide  — full Link: discoverable on every UP interface
    // Default at construction is Off — engines must opt in.
    enum class LinkVisibility { Off = 0, LoopbackOnly = 1, NetworkWide = 2 };
    void           setLinkVisibility(LinkVisibility v);
    LinkVisibility getLinkVisibility() const;

    // Whoever binds state to the session's current substrate (Link Audio's
    // sinks and subscriptions do) hears every visibility transition, in
    // order: linkWillChangeVisibility before the session is touched, so
    // substrate-bound state can drop; linkDidEnable once the session is
    // back up at a non-Off visibility, so it can be rebuilt. At most one
    // listener; null clears. Called on whichever thread drives the
    // transition (the app thread, or the session's deferred worker for an
    // async enable).
    struct LinkVisibilityListener {
        virtual ~LinkVisibilityListener() = default;
        virtual void linkWillChangeVisibility() = 0;
        virtual void linkDidEnable() = 0;
    };
    void setLinkVisibilityListener(LinkVisibilityListener* listener);

    // Peer name shown to other Link participants. Truncated to 256 chars
    // by Link. Stable NUL-terminated pointer valid until next setPeerName.
    void        setPeerName(const char* name);
    const char* peerName() const;

    // ─── Link peers ──────────────────────────────────────────────────────

    struct PeerInfo {
        std::string nodeId;            // 16-hex-char unique Link node identifier
        std::string gatewayIp;         // local interface they were discovered on
        std::string measurementIp;     // peer's ping/pong endpoint IP
        uint16_t    measurementPort{0};
        std::string audioIp;           // peer's Link Audio endpoint IP, "" if none
        uint16_t    audioPort{0};
        bool        isLoopback{false};
    };
    std::vector<PeerInfo> listPeers() const;

    // ─── Audio-thread API (RT-safe) ───────────────────────────────────────

    // `now()` is the app-thread read: returns the latest audio-thread NTP,
    // cached by the most recent update call. Both builds.
    //
    // `nowAt(audioCurrentTime)` is the WASM worklet's audio-thread entry
    // point: computes NTP from the supplied AudioContext currentTime and
    // publishes it to the cache. On native, audio-thread NTP comes from
    // the IIR — `nowAt` ignores its argument and returns `now()`.

    double now() const;
    double nowAt(double audioCurrentTime) const;

    // App-thread wall-clock NTP entry point. Native returns wallClockNTP()
    // directly; WASM has no wall clock and returns the last audio-thread NTP
    // (0 only before the first nowAt()).
    double wallNow() const;

    // Audio-thread time-base. Native runs one IIR step per callback;
    // WASM evaluates the SAB formula.
    double updateAudioThreadNTP(double samplePosition,
                                double sampleRate,
                                double audioCurrentTime = 0.0);
    void   resetAudioThreadTime(double samplePosition, double sampleRate);

    // Freewheel clock mode: derive the audio-thread NTP purely from sample
    // position, skipping the wall-clock drift IIR in updateAudioThreadNTP.
    // For deterministic offline/test rendering — the headless driver thread
    // can be preempted by the OS on a busy machine, and chasing that as
    // "drift" injects scheduling jitter that real hardware (driven by the
    // device callback) never sees. Off by default; real devices and the
    // headless fallback keep drift compensation.
    void   setFreewheelClock(bool enabled);

    // Mirror the current Link clock readouts (peers / tempo / beat / phase /
    // playing) into the dashboard metrics (relaxed atomics). RT-safe; called
    // once per callback. No-op on WASM. `m` may be null (skipped). Link Audio
    // stream health is LinkAudioHost::publishMetrics.
    void publishLinkMetrics(PerformanceMetrics* m, double quantum = 4.0);

    // Mirror the current ClockworkClock readout (tempo/beat/phase/playing) into the
    // cross-platform clock metrics (slots 46-49). Reads the ClockworkClockState SAB
    // mirror directly (relaxed atomics) + inline beat math — RT-safe and
    // identical on web and native, independent of Link. Called once per audio
    // callback with the audio-thread NTP. `m` may be null (skipped). Shared
    // implementation in ClockworkClock.cpp (no platform override).
    void publishClockMetrics(PerformanceMetrics* m, double ntpNow, double quantum = 4.0);

    // ─── Shared-memory state accessor (RT-safe reads) ────────────────────
    // Underlying ClockworkClockState — the engine's shared arena region on BOTH
    // WASM (bound at clockwork_clock_wasm_init) and native (bound at
    // bindStateToShm), so the clock has one identical SHM shape on every build.
    // Atomic loads through this pointer are RT-safe; getBpm()/isPlaying()
    // read the same atomics (ClockworkClock.cpp), so the audio thread can take
    // either.
    ClockworkClockState*       state();
    const ClockworkClockState* state() const;

    // Point the clock state at a shared arena's CLOCK_STATE region (copying
    // current state into it) so the SHM has one shape on every build. Native
    // binds the cross-process arena at engine init; the worklet (WASM) binds its
    // SAB region at boot via clockwork_clock_wasm_init. Called once before the audio
    // thread runs — no concurrency.
    void bindStateToShm(ClockworkClockState* region);
    // Copy the state back out of the arena and point at the private copy
    // again. Must be called before the arena is freed; safe to call twice.
    void unbindStateFromShm();

    // ─── Sample clock (engine sample position ↔ wall-clock DAC time) ─────
    // The clock's sample↔time line, published for cross-process readers: one
    // anchor (samplePosition, renderNtp + outputLatency/rate) plus the rate
    // defines dac_time(frame) for every frame, like the beat↔time
    // timelines above. Consumers (scope streams, any audible-time query)
    // convert cursors through it via sample_clock_view (shm_segment.hpp).
    // Region layout: SAMPLE_CLOCK_* in shared_memory.h. Bind once at engine
    // init; the driver publishes an anchor once per hardware callback and
    // calls advanceEngineFrames per rendered block, on the audio thread
    // (seqlock write, RT-safe, no allocation).
    void bindSampleClockToShm(uint8_t* region);
    void publishSampleClock(double samplePosition, double sampleRate,
                            double renderNtp, uint32_t outputLatencyFrames);
    // Advance the engine-frame counter that anchors scope-stream writes,
    // without republishing the anchor. Drivers that render several engine
    // blocks per publishSampleClock call this per block; the sample-clock
    // line is linear, so one anchor per hardware callback suffices.
    void advanceEngineFrames(double samplePosition);
    // Cumulative rendered engine frames (the advanceEngineFrames counter),
    // readable from any thread. The watchdog's rate-skew check compares its
    // advance against a monotonic clock to catch a device whose callbacks
    // tick at the wrong rate.
    uint64_t engineFrames() const;

#if CLOCKWORK_WORKLET_CLOCK
    // Worklet builds only: hand the worklet TimeSource its SAB offset pointers
    // (NTP start / drift µs / global ms) so nowAt() can evaluate the SAB time
    // formula. Wired by clockwork_clock_wasm_init at boot.
    void bindWorkletClock(const double* ntpStartTime,
                          const std::atomic<int32_t>* driftOffset,
                          const std::atomic<int32_t>* globalOffset);
#endif

    // The Link session itself, for the one borrower that needs more than the
    // NTP surface: LinkAudioHost, whose sinks share the session's Link
    // instance and whose block stamp is in Link's clock domain. Native only.
    LinkSession& linkSession();

private:
    // Sample-clock arena region (bindSampleClockToShm); null until bound —
    // publishSampleClock then skips the seqlock publish and only advances the
    // engine-frame counter (headless/unit contexts without a segment). Plain
    // pointer: bound once pre-audio, then single-writer.
    uint8_t* mSampleClockRegion = nullptr;
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// Active ClockworkClock pointer for the /clockwork/clock/* query verbs (queryable
// in both SAB and PM modes). Published at engine boot, single-publisher
// — multi-engine native is not supported.
extern std::atomic<ClockworkClock*> g_active_clockwork_clock;
