// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * LinkAudioHost.h — the engine's owner of the audio half of Ableton Link.
 *
 * Link is two things: a clock session (tempo, transport, peers) and an audio
 * mesh riding on it. The session is clock work and lives in ClockworkClock; this is
 * everything else — the publish gate and main sink, the aux sinks, the peer
 * channels and input subscriptions, and the block stamp in Link's own clock
 * domain. ClockworkEngine owns one of these beside its ClockworkClock, and hands it to
 * the audio drivers and the OSC control surface that used to reach through
 * the clock for it.
 *
 * It borrows the clock's Link session and never talks to Link on its own:
 * the sinks and subscriptions bind to the session's current substrate, so
 * they have to drop before the session goes down and come back after it is
 * up. That ordering is the clock's visibility transition, and the host hangs
 * off it as the clock's LinkVisibilityListener. The clock does not know what
 * the listener is; it only promises the two calls in order.
 *
 * The bridge underneath (LinkAudioBridge) is the compile-shape switch: real
 * when CLOCKWORK_LINK_AUDIO, an inline no-op otherwise. This class is compiled
 * either way, so every caller has one type to hold.
 */
#pragma once

#include "clock/ClockworkClock.h"
#include "native/LinkAudioBridge.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct PerformanceMetrics;

class LinkAudioHost : public ClockworkClock::LinkVisibilityListener {
public:
    // Registers as the clock's visibility listener; the clock must outlive
    // this object (ClockworkEngine declares them in that order).
    explicit LinkAudioHost(ClockworkClock& clock);
    ~LinkAudioHost() override;

    LinkAudioHost(const LinkAudioHost&) = delete;
    LinkAudioHost& operator=(const LinkAudioHost&) = delete;

    using Channel         = link_audio::Channel;
    using ConnectionState = link_audio::ConnectionState;
    using InputStatus     = link_audio::InputStatus;
    using SinkInfo        = link_audio::SinkInfo;
    static constexpr double kMaxInputLatencySeconds = link_audio::kMaxInputLatencySeconds;

    // ─── Publish gate ─────────────────────────────────────────────────────
    // Orthogonal to the clock's visibility — true advertises + sends our
    // channels on the mesh, false stays silent while the mesh is up. Default
    // false; explicit opt-in. Mirrored into the clock state's
    // SC_FLAG_LINK_AUDIO_PUBLISH so every reader of that region sees it.
    void setPublishEnabled(bool publish);
    bool isPublishEnabled() const;

    // ─── Peer channels + input subscriptions ──────────────────────────────
    std::vector<Channel> listChannels() const;

    // Subscribe to one peer's named audio channel; received samples arrive
    // on two consecutive input channels (busIdx, busIdx+1) — Link Audio caps
    // a channel at stereo and mono sources are mirrored. (peerName,
    // channelName) is the replacement key. True iff a matching channel was
    // found at call time.
    bool addInput(const char* peerName, const char* channelName, uint32_t busIdx);
    void removeInput(const char* peerName, const char* channelName);
    void clearInputs();
    // Per-subscription playback lookahead in seconds, like Live's per-track
    // latency slider. Rejected outside [0, kMaxInputLatencySeconds]. True iff
    // the subscription exists and the value is in range.
    bool setInputLatencySeconds(const char* peerName, const char* channelName,
                                double seconds);
    std::vector<InputStatus> listInputs() const;

    // ─── Auxiliary sinks ──────────────────────────────────────────────────
    // Named sinks beyond the main one, each tapping a bus range on the audio
    // thread; `name` is the replacement key.
    bool addSink(const char* name, uint32_t busIdx, uint32_t numChannels);
    void removeSink(const char* name);
    std::vector<SinkInfo> listSinks() const;

    // ─── Audio thread ─────────────────────────────────────────────────────
    // What a frame is: the endpoint resamples peers' streams onto our
    // timeline. Safe before any subscription exists; call again on a rate
    // change.
    // `allocatedInputChannels` is the width the DSP allocated (device inputs
    // plus reserved lanes), NOT the device's width — see channelPairFits.
    void setAudioFormat(uint32_t sampleRate, uint32_t allocatedInputChannels);

    // The block stamp, and the only place Link's own clock is spoken: Link
    // Audio aligns peers' streams on Link's per-boot micros, not NTP. Derived
    // from the sample counter and IIR-corrected toward the Link clock so a
    // late wake does not move it. 0 without Link (the bridge reads 0 as
    // "now"). Audio thread only.
    int64_t blockHostMicros(double samplePosition, double sampleRate);
    // Re-anchor on the next stamp — call wherever the driver resets the
    // clock's audio-thread time (device start, first manual pump).
    void resetBlockClock();

    // Tap each aux sink's bus range and publish. RT-safe (try_lock; skips
    // the block if the sink list is mid-mutation).
    void publishAuxSinks(const float* busPool, uint32_t blockSize, uint32_t numBuses,
                         uint32_t sampleRate, uint64_t hostMicrosForBufferBegin,
                         double quantum = 4.0);
    // Publish one main-sink block; stereo iff rightChannel != nullptr. True
    // iff a remote subscriber is present and audio shipped.
    bool publishAudioBlock(const float* leftChannel, const float* rightChannel,
                           size_t numFrames, uint32_t sampleRate,
                           uint64_t hostMicrosForBufferBegin = 0, double quantum = 4.0);

    // Mirror stream health (publish gate, sinks, underruns, input health)
    // into the dashboard metrics. RT-safe; once per callback. `m` may be null.
    void publishMetrics(PerformanceMetrics* m);

    // ─── ClockworkClock::LinkVisibilityListener ─────────────────────────────────
    void linkWillChangeVisibility() override;
    void linkDidEnable() override;

private:
    ClockworkClock&       mClock;
    LinkAudioBridge mBridge;

    // Block-clock anchor in Link's domain: sample-counter line + slow IIR,
    // the same scheme TimeSource uses for audio-thread NTP.
    double mHostBaseMicros{0.0};
    bool   mHostAnchored{false};
};
