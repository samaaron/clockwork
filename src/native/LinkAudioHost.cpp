// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#include "native/LinkAudioHost.h"

#include "clock/LinkSession.h"
#include "clock/clock_math.h"
#include "shared_memory.h"

#if CLOCKWORK_LINK_AUDIO
LinkAudioHost::LinkAudioHost(ClockworkClock& clock)
    : mClock(clock), mBridge(clock.linkSession().linkAudio()) {
    mClock.setLinkVisibilityListener(this);
}
#else
LinkAudioHost::LinkAudioHost(ClockworkClock& clock) : mClock(clock) {
    mClock.setLinkVisibilityListener(this);
}
#endif

LinkAudioHost::~LinkAudioHost() {
    mClock.setLinkVisibilityListener(nullptr);
}

// ─── Publish gate ───────────────────────────────────────────────────────────

void LinkAudioHost::setPublishEnabled(bool publish) {
    // The flag lives in the clock state for cross-build layout parity; the
    // sink machinery reflects immediately when the mesh is up, else the next
    // linkDidEnable creates it.
    mClock.state()->setFlag(SC_FLAG_LINK_AUDIO_PUBLISH, publish);
    mBridge.setPublishEnabled(publish, mClock.isLinkEnabled());
}

bool LinkAudioHost::isPublishEnabled() const {
    return mBridge.isPublishEnabled();
}

// ─── Channels, inputs, sinks (thin: the bridge owns them) ──────────────────

std::vector<LinkAudioHost::Channel> LinkAudioHost::listChannels() const {
    return mBridge.listChannels();
}

bool LinkAudioHost::addInput(const char* peerName, const char* channelName,
                             uint32_t busIdx) {
    return mBridge.addInput(peerName, channelName, busIdx);
}

void LinkAudioHost::removeInput(const char* peerName, const char* channelName) {
    mBridge.removeInput(peerName, channelName);
}

void LinkAudioHost::clearInputs() {
    mBridge.clearInputs();
}

bool LinkAudioHost::setInputLatencySeconds(const char* peerName, const char* channelName,
                                           double seconds) {
    return mBridge.setInputLatencySeconds(peerName, channelName, seconds);
}

std::vector<LinkAudioHost::InputStatus> LinkAudioHost::listInputs() const {
    return mBridge.listInputs();
}

bool LinkAudioHost::addSink(const char* name, uint32_t busIdx, uint32_t numChannels) {
    return mBridge.addSink(name, busIdx, numChannels);
}

void LinkAudioHost::removeSink(const char* name) {
    mBridge.removeSink(name);
}

std::vector<LinkAudioHost::SinkInfo> LinkAudioHost::listSinks() const {
    return mBridge.listSinks();
}

// ─── Audio thread ───────────────────────────────────────────────────────────

void LinkAudioHost::setAudioFormat(uint32_t sampleRate, uint32_t allocatedInputChannels) {
    mBridge.setAudioFormat(sampleRate, allocatedInputChannels);
}

// Reading the Link clock at audio-thread wake is jittery; the sample counter
// is not. Anchor the counter to the Link clock once, then let a slow IIR track
// real drift while rejecting wake jitter.
int64_t LinkAudioHost::blockHostMicros(double samplePosition, double sampleRate) {
    const int64_t linkClock = mClock.linkSession().linkClockMicrosRaw();
    if (linkClock == 0) return 0;
    if (sampleRate <= 0.0) return linkClock;
    const double sampleOffsetMicros = (samplePosition / sampleRate) * 1e6;
    const double linkNow = static_cast<double>(linkClock);

    if (!mHostAnchored) {
        mHostBaseMicros = linkNow - sampleOffsetMicros;
        mHostAnchored = true;
    } else {
        const double drift = linkNow - (mHostBaseMicros + sampleOffsetMicros);
        mHostBaseMicros += drift * clockwork::kDriftIirGain;
    }
    return static_cast<int64_t>(mHostBaseMicros + sampleOffsetMicros);
}

void LinkAudioHost::resetBlockClock() {
    mHostAnchored = false;
}

void LinkAudioHost::publishAuxSinks(const float* busPool, uint32_t blockSize,
                                    uint32_t numBuses, uint32_t sampleRate,
                                    uint64_t hostMicrosForBufferBegin, double quantum) {
    mBridge.publishAuxSinks(busPool, blockSize, numBuses, sampleRate,
                            hostMicrosForBufferBegin, quantum);
}

bool LinkAudioHost::publishAudioBlock(const float* leftChannel, const float* rightChannel,
                                      size_t numFrames, uint32_t sampleRate,
                                      uint64_t hostMicrosForBufferBegin, double quantum) {
    return mBridge.publishAudioBlock(leftChannel, rightChannel, numFrames, sampleRate,
                                     hostMicrosForBufferBegin, quantum);
}

void LinkAudioHost::publishMetrics(PerformanceMetrics* m) {
    if (!m) return;
    // The no-op bridge returns defaults / false try-reads, so these fields
    // hold their last value on a build without Link Audio.
    m->link_audio_publish.store(mBridge.isPublishEnabled() ? 1u : 0u,
                                std::memory_order_relaxed);
    uint32_t sinkCount = 0;
    if (mBridge.tryReadSinkCount(sinkCount))
        m->link_audio_sinks.store(sinkCount, std::memory_order_relaxed);
    m->link_audio_underruns.store(mBridge.underruns(), std::memory_order_relaxed);
    LinkAudioBridge::InputHealth health;
    if (mBridge.tryReadInputHealth(health)) {
        m->link_audio_in_channels.store(health.inChannels, std::memory_order_relaxed);
        m->link_audio_stream_rate.store(health.streamRate, std::memory_order_relaxed);
        m->link_audio_drift_ppm.store(health.driftPpm, std::memory_order_relaxed);
        m->link_audio_buffered_ms.store(health.bufferedMs, std::memory_order_relaxed);
    }
}

// ─── Visibility protocol ────────────────────────────────────────────────────
// Sinks and subscriptions bind to the session's current substrate
// (session-scoped channel ids), so they drop before the session moves and
// the main sink comes back — if publishing is on — once it is up again. The
// app re-adds inputs and aux sinks after a transition.

void LinkAudioHost::linkWillChangeVisibility() {
    mBridge.resetForVisibilityChange();
}

void LinkAudioHost::linkDidEnable() {
    mBridge.ensureMainSink();
}
