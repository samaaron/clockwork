// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * LinkAudioBridge.cpp — the audio side of Ableton Link (real impl).
 *
 * Compiled only when CLOCKWORK_LINK_AUDIO. Owns the publish-side main sink + aux
 * sinks, the receive-side input subscriptions, and the RT-thread publish
 * across Link's channels and the DSP's bus pool. The ableton::LinkAudio
 * instance is borrowed from the clock's LinkSession by reference; clock sync
 * (tempo/transport/peers) stays there, and LinkAudioHost owns this bridge.
 */
#include "native/LinkAudioBridge.h"

#if CLOCKWORK_LINK_AUDIO

#include <ableton/util/FloatIntConversion.hpp>

#include "clockwork_config.h"   // clockwork_log

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace {

// Render an 8-byte Link NodeId/PeerId/ChannelId as 16 lowercase hex.
template <typename Bytes>
inline std::string bytesToHex16(const Bytes& bytes) {
    char buf[17];
    for (size_t k = 0; k < bytes.size(); ++k) {
        std::snprintf(buf + 2 * k, 3, "%02x", bytes[k]);
    }
    return std::string(buf, 16);
}

// Use the caller-supplied audio-framework timestamp if available; otherwise
// fall back to link.clock().micros() (jittery; testing only).
inline std::chrono::microseconds hostMicrosOrNow(
    uint64_t hostMicros, const ableton::LinkAudio& link) {
    return hostMicros > 0
        ? std::chrono::microseconds{static_cast<int64_t>(hostMicros)}
        : link.clock().micros();
}

// Interleave two float channels (R nullable for mono) into one int16 buffer
// with Link's saturating float→int16 conversion.
inline void interleaveFloatToInt16(const float* L, const float* R,
                                   int16_t* dst, size_t numFrames) {
    if (R) {
        for (size_t i = 0; i < numFrames; ++i) {
            dst[2 * i]     = ableton::util::floatToInt16(L[i]);
            dst[2 * i + 1] = ableton::util::floatToInt16(R[i]);
        }
    } else {
        for (size_t i = 0; i < numFrames; ++i) {
            dst[i] = ableton::util::floatToInt16(L[i]);
        }
    }
}

}  // namespace

// ─── Publish gate + channel discovery ────────────────────────────────────

void LinkAudioBridge::setPublishEnabled(bool publish, bool linkEnabled) {
    if (mAudioPublishEnabled == publish) return;
    mAudioPublishEnabled = publish;

    // Reflect immediately if the Link mesh is already up; otherwise the next
    // ensureMainSink (on setLinkVisibility(non-Off)) creates the sink.
    // LinkAudio stays enabled in both directions — we keep observing peers'
    // channels even after we stop publishing.
    if (!linkEnabled) return;
    if (publish) {
        std::lock_guard<std::mutex> lk(mSinkMutex);
        mSink.emplace(mLink, mChannelNameCache, kSinkMaxSamples);
    } else {
        // Aux sinks share the publish-side substrate — clear them when publish
        // goes off; the main sink stops broadcasting too.
        {
            std::lock_guard<std::mutex> auxLk(mAuxSinksMutex);
            mAuxSinks.clear();
            mAuxSinkCount.store(0, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lk(mSinkMutex);
            if (mSink) mSink.reset();
        }
    }
}

std::vector<link_audio::Channel> LinkAudioBridge::listChannels() const {
    std::vector<link_audio::Channel> out;
    auto channels = mLink.channels();
    out.reserve(channels.size());
    for (auto& c : channels) {
        out.push_back({bytesToHex16(c.id), c.name,
                       bytesToHex16(c.peerId), c.peerName});
    }
    return out;
}

// ─── Input subscriptions ──────────────────────────────────────────────────

bool LinkAudioBridge::addInput(const char* peerName, const char* channelName,
                               uint32_t busIdx) {
    if (!peerName || !channelName) return false;

    auto channels = mLink.channels();
    auto match = std::find_if(channels.begin(), channels.end(),
        [&](const auto& c) {
            return c.peerName == peerName && c.name == channelName;
        });
    if (match == channels.end()) {
        clockwork_log("[link-audio] no channel '%s' published by peer '%s'",
                channelName, peerName);
        return false;
    }

    /*
     * THE PAIR HAS TO FIT THE INPUT CHANNELS, and nothing else checks it.
     *
     * clockwork_port_bus_attach binds whatever it is given — it refuses a double
     * attach and a full table, and takes no view on how many channels the
     * engine opened. pull_port_sources then skips a binding that starts past
     * the end, so a subscription bound out of range reports success and is
     * silent for ever: the exact shape of failure a refusal exists to prevent.
     *
     * The width is the ALLOCATED one — device inputs plus the reserved lanes
     * above them — because that is what pull_port_sources bounds against. A
     * client subscribes into a reserved lane, which sits above every device
     * channel, so measuring against the device refuses every real request.
     *
     * Checked on every add rather than once, because Link streams arrive and
     * leave: the question is not "is there room" in the abstract but "is there
     * room for THIS pair, now".
     */
    const uint32_t inChannels = mInputChannels.load(std::memory_order_relaxed);
    if (!link_audio::channelPairFits(busIdx, inChannels)) {
        clockwork_log("[link-audio] channel pair %u/%u does not fit %u allocated "
                "input channels", busIdx, busIdx + 1, inChannels);
        return false;
    }
    const ableton::ChannelId newChannelId = match->id;

    // First pass: re-arm short-circuit (matching channelId reuses the existing
    // renderer + counters). Subscribe stays outside the lock — Link's
    // source-callback interacts with its own threading.
    {
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        InputSubscription* existing = nullptr;
        for (auto& s : mInputSubs) {
            if (s.peerName == peerName && s.channelName == channelName) {
                existing = &s;
                continue;
            }
            const uint32_t sLo = s.busIdx;
            const uint32_t sHi = s.busIdx + 1;
            const uint32_t nLo = busIdx;
            const uint32_t nHi = busIdx + 1;
            if (sLo <= nHi && nLo <= sHi) {
                clockwork_log("[link-audio] channel pair %u/%u overlaps %s/%s at %u/%u",
                        nLo, nHi, s.peerName.c_str(), s.channelName.c_str(), sLo, sHi);
                return false;
            }
        }
        if (existing && existing->channelId == newChannelId) {
            // Re-arm of the same channel at a possibly different pair. The
            // BINDING is what decides where the audio lands, so moving busIdx
            // without moving it would leave the stream on the old channels
            // while every status reply named the new ones.
            if (existing->busIdx != busIdx && existing->port != CLOCKWORK_PORT_NONE) {
                clockwork_port_bus_detach(existing->port);
                if (!clockwork_port_bus_attach(existing->port, busIdx)) {
                    clockwork_log("[link-audio] could not move %s/%s to channel %u",
                            peerName, channelName, busIdx);
                    return false;
                }
            }
            existing->busIdx = busIdx;
            return true;
        }
    }
    // Build a fresh renderer outside the lock.
    InputSubscription sub;
    sub.busIdx      = busIdx;
    sub.peerName    = peerName;
    sub.channelName = channelName;
    sub.channelId   = newChannelId;
    sub.renderer = std::make_unique<
        clockwork_link::LinkAudioInputRenderer<ableton::LinkAudio>>(mLink);
    sub.renderer->subscribe(newChannelId);

    // The port this peer's audio arrives through, bound to the channel pair
    // the caller asked for. Stereo always: the renderer mirrors a mono source
    // to both, so the DSP sees a consistent shape whatever the peer publishes.
    // Named for the peer and channel so a port listing says whose audio it is.
    const std::string portName = std::string("link:") + peerName + "/" + channelName;
    sub.port = clockwork_port_open(portName.c_str(), kClockworkPortSource, 2,
                             kInputPortCapacityFrames);
    // An unbound port is NOT an acceptable subscription: it would fill, stall,
    // and read to a client exactly like one that works.
    if (sub.port == CLOCKWORK_PORT_NONE) {
        clockwork_log("[link-audio] no port available for %s/%s", peerName, channelName);
        return false;
    }
    if (!clockwork_port_bus_attach(sub.port, busIdx)) {
        clockwork_log("[link-audio] could not bind %s/%s to channel %u",
                peerName, channelName, busIdx);
        clockwork_port_close(sub.port);
        sub.port = CLOCKWORK_PORT_NONE;
        return false;
    }

    std::lock_guard<std::mutex> lk(mInputSubMutex);
    // Re-validate under the lock; state may have shifted while we were building.
    InputSubscription* replaceSlot = nullptr;
    for (auto& s : mInputSubs) {
        if (s.peerName == peerName && s.channelName == channelName) {
            replaceSlot = &s;
            continue;
        }
        const uint32_t sLo = s.busIdx;
        const uint32_t sHi = s.busIdx + 1;
        const uint32_t nLo = busIdx;
        const uint32_t nHi = busIdx + 1;
        if (sLo <= nHi && nLo <= sHi) return false;
    }
    if (replaceSlot) {
        closePort(*replaceSlot);
        *replaceSlot = std::move(sub);
        return true;
    }
    mInputSubs.push_back(std::move(sub));
    mInputSubCount.store(mInputSubs.size(), std::memory_order_relaxed);
    // First subscription: the worker has something to do now.
    if (mSampleRate.load(std::memory_order_relaxed) != 0) startEndpoint();
    return true;
}

// Detach before close, always: the binding names the slot, and a slot closed
// while still bound would leave the bus reading a port that no longer exists.
void LinkAudioBridge::closePort(InputSubscription& sub) {
    if (sub.port == CLOCKWORK_PORT_NONE) return;
    clockwork_port_bus_detach(sub.port);
    clockwork_port_close(sub.port);
    sub.port = CLOCKWORK_PORT_NONE;
}

void LinkAudioBridge::removeInput(const char* peerName, const char* channelName) {
    if (!peerName || !channelName) return;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    auto& v = mInputSubs;
    for (auto& s : v)
        if (s.peerName == peerName && s.channelName == channelName) closePort(s);
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& s) {
                return s.peerName == peerName && s.channelName == channelName;
            }),
            v.end());
    mInputSubCount.store(v.size(), std::memory_order_relaxed);
}

void LinkAudioBridge::clearInputs() {
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    for (auto& s : mInputSubs) closePort(s);
    mInputSubs.clear();
    mInputSubCount.store(0, std::memory_order_relaxed);
}

bool LinkAudioBridge::setInputLatencySeconds(const char* peerName,
                                             const char* channelName,
                                             double seconds) {
    if (!peerName || !channelName) return false;
    if (!(seconds >= 0.0) ||
        seconds > link_audio::kMaxInputLatencySeconds) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    for (auto& sub : mInputSubs) {
        if (sub.peerName == peerName && sub.channelName == channelName) {
            sub.renderer->setLatencySeconds(seconds);
            return true;
        }
    }
    return false;
}

std::vector<link_audio::InputStatus> LinkAudioBridge::listInputs() const {
    std::vector<link_audio::InputStatus> out;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    out.reserve(mInputSubs.size());
    for (const auto& sub : mInputSubs) {
        link_audio::InputStatus s;
        s.peerName          = sub.peerName;
        s.channelName       = sub.channelName;
        s.busIdx            = sub.busIdx;
        s.sampleRate        = sub.renderer->lastSampleRate();
        s.sourceNumChannels = sub.renderer->lastNumChannels();
        s.bufferedSeconds      = sub.renderer->bufferedSeconds();
        s.droppedSourceBuffers    = sub.renderer->droppedSourceBuffers();
        s.networkGapBuffers       = sub.renderer->networkGapBuffers();
        s.totalSourceBufferCalls  = sub.renderer->totalSourceBufferCalls();
        s.duplicateCountCalls     = sub.renderer->duplicateCountCalls();
        s.latencySeconds          = sub.renderer->latencySeconds();

        /*
         * THE BUFFER IS IN TWO PLACES NOW, so health has to count both.
         *
         * bufferedSeconds is what the RENDERER holds — peer buffers its
         * network thread has delivered and it has not yet resampled. That used
         * to be the whole of it, because the audio thread pulled straight from
         * the renderer one block at a time. The endpoint drains it into the
         * port ring as fast as there is room, so the renderer now sits near
         * empty while the stream is perfectly healthy, and a subscription that
         * was working reported Dropout for ever.
         *
         * What a client wants to know is how much audio stands between it and
         * a gap, which is both stages.
         */
        const double portQueuedSeconds =
            (sub.port != CLOCKWORK_PORT_NONE && mSampleRate.load(std::memory_order_relaxed) != 0)
                ? double(clockwork_port_readable(sub.port))
                      / double(mSampleRate.load(std::memory_order_relaxed))
                : 0.0;
        s.bufferedSeconds += static_cast<float>(portQueuedSeconds);

        constexpr float kMinHealthyBufferSeconds = 0.005f;
        const bool everReceived = sub.renderer->everReceived();
        if (!everReceived) {
            s.state = link_audio::ConnectionState::Connecting;
        } else if (s.bufferedSeconds < kMinHealthyBufferSeconds) {
            s.state = link_audio::ConnectionState::Dropout;
        } else {
            s.state = link_audio::ConnectionState::Connected;
        }
        out.push_back(std::move(s));
    }
    return out;
}

// ─── Auxiliary sinks ──────────────────────────────────────────────────────

bool LinkAudioBridge::addSink(const char* name, uint32_t busIdx,
                              uint32_t numChannels) {
    if (!name || numChannels == 0 || numChannels > 2) return false;
    // Construct outside the lock — LinkAudioSink ctor isn't RT-safe but we're
    // on the app thread, and we don't want it inside the lock.
    ActiveSink entry{
        std::string(name),
        busIdx,
        numChannels,
        ableton::LinkAudioSink(mLink, std::string(name), kSinkMaxSamples)
    };
    std::lock_guard<std::mutex> lk(mAuxSinksMutex);
    for (auto& as : mAuxSinks) {
        if (as.name == name) {
            as = std::move(entry);
            return true;
        }
    }
    mAuxSinks.push_back(std::move(entry));
    mAuxSinkCount.store(mAuxSinks.size(), std::memory_order_relaxed);
    return true;
}

void LinkAudioBridge::removeSink(const char* name) {
    if (!name) return;
    std::lock_guard<std::mutex> lk(mAuxSinksMutex);
    auto& v = mAuxSinks;
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& as) { return as.name == name; }),
            v.end());
    mAuxSinkCount.store(v.size(), std::memory_order_relaxed);
}

std::vector<link_audio::SinkInfo> LinkAudioBridge::listSinks() const {
    std::vector<link_audio::SinkInfo> out;
    std::lock_guard<std::mutex> lk(mAuxSinksMutex);
    out.reserve(mAuxSinks.size());
    for (const auto& as : mAuxSinks) {
        out.push_back({as.name, as.busIdx, as.numChannels, as.hasSubscriber});
    }
    return out;
}

// ─── RT-thread: publish + drain (RT-safe) ─────────────────────────────────

void LinkAudioBridge::publishAuxSinks(const float* busPool, uint32_t blockSize,
                                      uint32_t numBuses, uint32_t sampleRate,
                                      uint64_t hostMicrosForBufferBegin,
                                      double quantum) {
    if (!busPool) return;
    // Fast path: skip the mutex entirely when there are no aux sinks.
    if (mAuxSinkCount.load(std::memory_order_relaxed) == 0) return;
    // try_lock keeps the audio thread RT-friendly: skip block on contention,
    // next one recovers.
    std::unique_lock<std::mutex> lk(mAuxSinksMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (mAuxSinks.empty()) return;

    auto sessionState = mLink.captureAudioSessionState();
    const auto hostMicros = hostMicrosOrNow(hostMicrosForBufferBegin, mLink);
    const double beatsAtBegin = sessionState.beatAtTime(hostMicros, quantum);

    for (auto& as : mAuxSinks) {
        if (as.busIdx + as.numChannels > numBuses) continue;
        ableton::LinkAudioSink::BufferHandle buf(as.sink);
        const bool subscribed = static_cast<bool>(buf);
        if (subscribed != as.hasSubscriber) as.hasSubscriber = subscribed;
        if (!subscribed) continue;
        if (blockSize * as.numChannels > buf.maxNumSamples) continue;

        const float* L = busPool + as.busIdx * blockSize;
        const float* R = as.numChannels == 2
            ? busPool + (as.busIdx + 1) * blockSize : nullptr;
        interleaveFloatToInt16(L, R, buf.samples, blockSize);
        buf.commit(sessionState, beatsAtBegin, quantum,
                   blockSize, as.numChannels, sampleRate);
    }
}

void LinkAudioBridge::setAudioFormat(uint32_t sampleRate, uint32_t allocatedInputChannels) {
    mSampleRate.store(sampleRate, std::memory_order_relaxed);
    mInputChannels.store(allocatedInputChannels, std::memory_order_relaxed);
    if (sampleRate != 0 && mInputSubCount.load(std::memory_order_relaxed) != 0)
        startEndpoint();
}

void LinkAudioBridge::startEndpoint() {
    if (mEndpointRun.exchange(true, std::memory_order_acq_rel)) return;
    mEndpointThread = std::thread([this] { endpointLoop(); });
}

void LinkAudioBridge::stopEndpoint() {
    if (!mEndpointRun.exchange(false, std::memory_order_acq_rel)) return;
    if (mEndpointThread.joinable()) mEndpointThread.join();
}

/*
 * Fill every subscription's port with as much as it has room for.
 *
 * THE HOST TIME IS THE ONE FOR THE FRAMES BEING WRITTEN, not for now: what is
 * already queued plays first, so these frames are heard that much later. The
 * renderer turns that into a beat range and reads the peer's buffers, which
 * carry beat ranges of their own — which is why running ahead of the audio
 * thread is safe. A tempo change moves time-per-beat for everyone at once, so
 * queued audio stays where it belongs on the timeline.
 *
 * Short reads are NOT padded. A renderer that had less than was asked for
 * means the peer's queue is dry, and producing zeros would fill the ring with
 * silence the port could not tell from audio — the underrun would vanish from
 * the one counter that reports it. Offering only what is real lets the ring
 * run down and the audio thread read silence, counted, as the substrate
 * intends.
 */
void LinkAudioBridge::endpointLoop() {
    using namespace std::chrono_literals;
    constexpr double kQuantum = 4.0;

    while (mEndpointRun.load(std::memory_order_acquire)) {
        const uint32_t sr = mSampleRate.load(std::memory_order_relaxed);
        if (sr == 0) { std::this_thread::sleep_for(2ms); continue; }

        bool produced = false;
        {
            // A blocking lock, not a try_lock: this is not the audio thread,
            // and skipping a pass on contention would starve the ring for no
            // benefit.
            std::lock_guard<std::mutex> lk(mInputSubMutex);
            if (!mInputSubs.empty()) {
                auto sessionState = mLink.captureAppSessionState();
                for (auto& sub : mInputSubs) {
                    if (sub.port == CLOCKWORK_PORT_NONE || !sub.renderer) continue;

                    uint32_t room = clockwork_port_writable(sub.port);
                    if (room == 0) continue;
                    if (room > kEndpointChunkFrames) room = kEndpointChunkFrames;

                    const uint32_t queued = clockwork_port_readable(sub.port);
                    const auto ahead = std::chrono::microseconds(
                        static_cast<long long>(1e6 * double(queued) / double(sr)));
                    const auto hostTime = mLink.clock().micros() + ahead;

                    const size_t filled = sub.renderer->receive(
                        mScratchL, mScratchR, room, sessionState,
                        static_cast<double>(sr), hostTime, kQuantum);
                    if (filled == 0) continue;

                    for (size_t i = 0; i < filled; ++i) {
                        mScratchInterleaved[2 * i]     = static_cast<float>(mScratchL[i]);
                        mScratchInterleaved[2 * i + 1] = static_cast<float>(mScratchR[i]);
                    }
                    clockwork_port_produce(sub.port, mScratchInterleaved,
                                     static_cast<uint32_t>(filled));
                    produced = true;
                }
            }
        }
        // Nothing to do means the rings are full or the peers are quiet.
        // Polling rather than waiting to be signalled, for the same reason the
        // disk reader does: being signalled would mean the audio thread doing
        // the signalling.
        if (!produced) std::this_thread::sleep_for(1ms);
    }
}

bool LinkAudioBridge::publishAudioBlock(const float* leftChannel,
                                        const float* rightChannel,
                                        size_t numFrames, uint32_t sampleRate,
                                        uint64_t hostMicrosForBufferBegin,
                                        double quantum) {
    // try_lock: skip the publish if setLinkVisibility / setPublishEnabled is
    // mid-reset of mSink.
    std::unique_lock<std::mutex> lk(mSinkMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    if (!mSink) return false;
    ableton::LinkAudioSink::BufferHandle buf(*mSink);
    if (!buf) return false;

    const size_t numChannels = rightChannel ? 2u : 1u;
    if (numFrames * numChannels > buf.maxNumSamples) return false;

    interleaveFloatToInt16(leftChannel, rightChannel, buf.samples, numFrames);

    const auto hostMicros = hostMicrosOrNow(hostMicrosForBufferBegin, mLink);
    auto st = mLink.captureAudioSessionState();
    const double beatsAtBegin = st.beatAtTime(hostMicros, quantum);

    return buf.commit(st, beatsAtBegin, quantum,
                      numFrames, numChannels, sampleRate);
}

// ─── Visibility-change support ────────────────────────────────────────────

void LinkAudioBridge::resetForVisibilityChange() {
    {
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        mInputSubs.clear();
        mInputSubCount.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> auxLk(mAuxSinksMutex);
        mAuxSinks.clear();
        mAuxSinkCount.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(mSinkMutex);
        if (mSink) mSink.reset();
    }
}

void LinkAudioBridge::ensureMainSink() {
    if (!mAudioPublishEnabled) return;
    std::lock_guard<std::mutex> lk(mSinkMutex);
    mSink.emplace(mLink, mChannelNameCache, kSinkMaxSamples);
}

// ─── Metrics ──────────────────────────────────────────────────────────────

bool LinkAudioBridge::tryReadSinkCount(uint32_t& outCount) const {
    std::unique_lock<std::mutex> lk(mAuxSinksMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    outCount = static_cast<uint32_t>(mAuxSinks.size());
    return true;
}

bool LinkAudioBridge::tryReadInputHealth(InputHealth& out) const {
    std::unique_lock<std::mutex> lk(mInputSubMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    uint32_t inCh = 0, rate = 0;
    int32_t  drift = 0;
    float    bufMs = 0.0f;
    for (auto& sub : mInputSubs) {
        inCh += sub.renderer->lastNumChannels();
        rate  = sub.renderer->lastSampleRate();
        drift = sub.renderer->lastDriftPpm();
        bufMs = std::max(bufMs, sub.renderer->bufferedSeconds() * 1000.0f);
    }
    out.inChannels = inCh;
    out.streamRate = rate;
    out.driftPpm   = drift;
    out.bufferedMs = static_cast<uint32_t>(bufMs);
    return true;
}

#endif  // CLOCKWORK_LINK_AUDIO
