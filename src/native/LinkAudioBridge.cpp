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

#include <ableton/link_audio/AudioBuffer.hpp>
#include <ableton/link_audio/BeatTimeMapping.hpp>
#include <ableton/link_audio/Resizer.hpp>
#include <ableton/link_audio/v1/Messages.hpp>
#include <ableton/util/FloatIntConversion.hpp>

#include "clockwork_config.h"   // clockwork_log
#include "lanes/lanes.h"         // clockwork_link_audio_lane_base
#include "workers/thread_name.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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

// ─── A stand-in peer ──────────────────────────────────────────────────────

/*
 * A stand-in channel: its name and ids, and Link's own Resizer, which cuts
 * the peer's audio into the chunks a Link peer's sink sends, a wire message's
 * worth at a time, each stamped on from the last. What the Resizer hands on
 * is what Link's decoder delivers, so each chunk becomes one buffer for the
 * subscriptions, made as Link's source makes it (LinkAudio.ipp).
 */
struct LinkAudioBridge::StandInChannel {
    // The audio a Link peer's message carries (Encoder::kMaxAudioBytes).
    static constexpr size_t kWireAudioBytes =
        576 - ableton::link_audio::v1::kHeaderSize - ableton::link_audio::AudioBuffer::kNonAudioBytes;

    struct Deliver {
        LinkAudioBridge*   bridge;
        ableton::ChannelId id;
        void operator()(int16_t* samples, const ableton::link_audio::AudioBuffer::Chunks& chunks,
                        uint32_t numChannels, uint32_t sampleRate,
                        const ableton::link_audio::Id& sessionId) {
            for (const auto& chunk : chunks) {
                ableton::LinkAudioSource::BufferHandle bh{};
                bh.samples              = samples;
                bh.info.numChannels     = numChannels;
                bh.info.numFrames       = chunk.numFrames;
                bh.info.sampleRate      = sampleRate;
                bh.info.count           = chunk.count;
                bh.info.sessionBeatTime = chunk.beginBeats.floating();
                bh.info.tempo           = chunk.tempo.bpm();
                bh.info.sessionId       = sessionId;
                for (auto& sub : bridge->mInputSubs)
                    if (sub.standIn && sub.channelId == id && sub.renderer) sub.renderer->push(bh);
                samples += size_t(chunk.numFrames) * numChannels;
            }
        }
    };

    StandInChannel(LinkAudioBridge* bridge, std::string peer, std::string channel,
                   ableton::ChannelId channelId, ableton::PeerId peer_)
        : peerName(std::move(peer)), channelName(std::move(channel)), id(channelId), peerId(peer_),
          resizer(ableton::util::injectVal(Deliver{bridge, channelId})) {}

    std::string          peerName, channelName;
    ableton::ChannelId   id;
    ableton::PeerId      peerId;
    std::vector<int16_t> samples;
    ableton::link_audio::Resizer<int16_t, Deliver, kWireAudioBytes> resizer;
};

LinkAudioBridge::LinkAudioBridge(ableton::LinkAudio& link) : mLink(link) {}

LinkAudioBridge::~LinkAudioBridge() {
    stopEndpoint();
    clearInputs();
}

const LinkAudioBridge::StandInChannel* LinkAudioBridge::standInNamed(const char* peerName,
                                                                     const char* channelName) const {
    for (const auto& c : mStandIns)
        if (c->peerName == peerName && c->channelName == channelName) return c.get();
    return nullptr;
}

void LinkAudioBridge::addStandInChannel(const char* peerName, const char* channelName) {
    if (!peerName || !channelName) return;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    if (standInNamed(peerName, channelName)) return;
    // Ids no Link peer makes: Link's are random, these count up after a
    // marker, and a peer's channels share its id.
    ableton::PeerId peerId{};
    peerId[0] = 0x5a; peerId[1] = 0x01;
    peerId[7] = static_cast<uint8_t>(mStandIns.size());
    for (const auto& c : mStandIns)
        if (c->peerName == peerName) peerId = c->peerId;
    ableton::ChannelId channelId{};
    channelId[0] = 0x5a; channelId[1] = 0x02;
    channelId[7] = static_cast<uint8_t>(mStandIns.size());
    mStandIns.push_back(std::make_unique<StandInChannel>(this, peerName, channelName, channelId, peerId));
}

bool LinkAudioBridge::sendStandInAudio(const char* peerName, const char* channelName,
                                       const float* interleaved, uint32_t numFrames,
                                       uint32_t numChannels, uint32_t sampleRate,
                                       uint64_t hostMicros) {
    if (!peerName || !channelName || !interleaved || numFrames == 0
        || (numChannels != 1 && numChannels != 2) || sampleRate == 0)
        return false;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    StandInChannel* c = nullptr;
    for (auto& s : mStandIns)
        if (s->peerName == peerName && s->channelName == channelName) c = s.get();
    if (!c) return false;
    // A Link peer sends to its receivers only, so its Resizer is not fed
    // while nothing subscribes.
    const bool heard = std::any_of(mInputSubs.begin(), mInputSubs.end(),
        [&](const auto& s) { return s.standIn && s.channelId == c->id; });
    if (!heard) return true;
    // As a Link peer's sink commits (LinkAudio.ipp, Sink.hpp): int16 samples,
    // the first frame's beat on the session as a global beat, and the
    // session's tempo and id.
    c->samples.resize(size_t(numFrames) * numChannels);
    for (size_t i = 0; i < c->samples.size(); ++i)
        c->samples[i] = ableton::util::floatToInt16(interleaved[i]);
    constexpr double kQuantum = 4.0;
    const auto state = mLink.captureAppSessionState();
    const auto& api  = ableton::detail::linkApiState(state);
    const double beats =
        state.beatAtTime(std::chrono::microseconds(static_cast<int64_t>(hostMicros)), kQuantum);
    c->resizer(c->samples.data(), numFrames, numChannels, sampleRate,
               ableton::link_audio::globalBeatAtBeat(api.timeline, ableton::link::Beats{beats},
                                                     ableton::link::Beats{kQuantum}),
               api.timeline.tempo, api.timelineSessionId);
    return true;
}

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
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    for (const auto& c : mStandIns)
        out.push_back({bytesToHex16(c->id), c->channelName, bytesToHex16(c->peerId), c->peerName});
    return out;
}

void LinkAudioBridge::reserveLanes(ClockworkCore* core) { clockwork_reserve_link_audio_lanes(core, kLanes); }

// ─── Input subscriptions ──────────────────────────────────────────────────

std::optional<uint32_t> LinkAudioBridge::addInput(const char* peerName, const char* channelName) {
    if (!peerName || !channelName) return std::nullopt;

    auto channels = mLink.channels();
    auto match = std::find_if(channels.begin(), channels.end(),
        [&](const auto& c) {
            return c.peerName == peerName && c.name == channelName;
        });
    std::optional<ableton::ChannelId> standInId;
    if (match == channels.end()) {
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        if (const auto* c = standInNamed(peerName, channelName)) standInId = c->id;
    }
    if (match == channels.end() && !standInId) {
        clockwork_log(core(), "[link-audio] no channel '%s' published by peer '%s'",
                channelName, peerName);
        return std::nullopt;
    }
    const ableton::ChannelId newChannelId = standInId ? *standInId : match->id;

    /*
     * THE BRIDGE CHOOSES THE PAIR. A client cannot: it does not know where
     * the Link Audio lanes are, and they move up when a wider device arrives.
     * A subscription that is already here keeps its pair; a new one gets the
     * first the others do not hold.
     */
    uint32_t busIdx = 0;
    {
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        std::vector<uint32_t> held;
        const InputSubscription* existing = nullptr;
        for (const auto& s : mInputSubs) {
            if (s.peerName == peerName && s.channelName == channelName) existing = &s;
            else held.push_back(s.busIdx);
        }
        // The same stream, already flowing.
        if (existing && existing->channelId == newChannelId) return existing->busIdx;
        const auto pair = existing ? std::optional<uint32_t>(existing->busIdx)
                                   : link_audio::firstFreePair(mLaneBase, mLanes, held);
        if (!pair) {
            if (mLanes == 0)
                clockwork_log(core(), "[link-audio] no Link Audio lanes reserved, so %s/%s has "
                        "nowhere to arrive", peerName, channelName);
            else
                clockwork_log(core(), "[link-audio] no channel pair free for %s/%s: the %u Link "
                        "Audio lanes are all in use", peerName, channelName, mLanes);
            return std::nullopt;
        }
        busIdx = *pair;
    }

    // The lanes sit inside the allocated width by construction, but a pair
    // outside it would be skipped by pull_port_sources and silent for ever,
    // the exact shape of failure a refusal exists to prevent.
    const uint32_t inChannels = mInputChannels.load(std::memory_order_relaxed);
    if (!link_audio::channelPairFits(busIdx, inChannels)) {
        clockwork_log(core(), "[link-audio] channel pair %u/%u does not fit %u allocated "
                "input channels", busIdx, busIdx + 1, inChannels);
        return std::nullopt;
    }

    // Build a fresh renderer outside the lock: Link's source callback
    // interacts with its own threading.
    InputSubscription sub;
    sub.busIdx      = busIdx;
    sub.peerName    = peerName;
    sub.channelName = channelName;
    sub.channelId   = newChannelId;
    sub.standIn     = standInId.has_value();
    sub.renderer = std::make_unique<
        clockwork_link::LinkAudioInputRenderer<ableton::LinkAudio>>(mLink, &mEndpointWake);
    if (!sub.standIn) sub.renderer->subscribe(newChannelId);

    // The port this peer's audio arrives through, bound to the pair. Stereo
    // always: the renderer mirrors a mono source to both, so the DSP sees a
    // consistent shape whatever the peer publishes. Named for the peer and
    // channel so a port listing says whose audio it is.
    const std::string portName = std::string("link:") + peerName + "/" + channelName;
    sub.port = clockwork_port_open(ports(), portName.c_str(), kClockworkPortSource, 2,
                             kInputPortCapacityFrames);
    // An unbound port is NOT an acceptable subscription: it would fill, stall,
    // and read to a client exactly like one that works.
    if (sub.port == CLOCKWORK_PORT_NONE) {
        clockwork_log(core(), "[link-audio] no port available for %s/%s", peerName, channelName);
        return std::nullopt;
    }
    if (!clockwork_port_bus_attach(mCore.load(std::memory_order_acquire), sub.port, busIdx)) {
        clockwork_log(core(), "[link-audio] could not bind %s/%s to channel %u",
                peerName, channelName, busIdx);
        clockwork_port_close(ports(), sub.port);
        sub.port = CLOCKWORK_PORT_NONE;
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lk(mInputSubMutex);
    // Re-check under the lock: another add may have taken the pair meanwhile.
    InputSubscription* replaceSlot = nullptr;
    for (auto& s : mInputSubs) {
        if (s.peerName == peerName && s.channelName == channelName) replaceSlot = &s;
        else if (s.busIdx == busIdx) {
            clockwork_log(core(), "[link-audio] channel pair %u/%u was taken while %s/%s subscribed",
                    busIdx, busIdx + 1, peerName, channelName);
            endInput(sub);
            return std::nullopt;
        }
    }
    if (replaceSlot) {
        endInput(*replaceSlot);
        *replaceSlot = std::move(sub);
        dateReadsOf(busIdx, replaceSlot->port);
        clockwork::bump(mEndpointWake);
        return busIdx;
    }
    mInputSubs.push_back(std::move(sub));
    dateReadsOf(busIdx, mInputSubs.back().port);
    mInputSubCount.store(mInputSubs.size(), std::memory_order_relaxed);
    // First subscription: the worker has something to do now.
    if (mSampleRate.load(std::memory_order_relaxed) != 0) startEndpoint();
    clockwork::bump(mEndpointWake);
    return busIdx;
}

// The port is detached before it is closed, always: the binding names the
// slot, and a slot closed while still bound would leave the bus reading a port
// that no longer exists. The count first, while the port can still say it.
void LinkAudioBridge::endInput(InputSubscription& sub) {
    mRetiredUnderruns += underrunsSinceStart(sub);
    if (sub.port == CLOCKWORK_PORT_NONE) return;
    forgetReadDates(sub.port);
    clockwork_port_bus_detach(mCore.load(std::memory_order_acquire), sub.port);
    clockwork_port_close(ports(), sub.port);
    sub.port = CLOCKWORK_PORT_NONE;
}

void LinkAudioBridge::removeInput(const char* peerName, const char* channelName) {
    if (!peerName || !channelName) return;
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    auto& v = mInputSubs;
    for (auto& s : v)
        if (s.peerName == peerName && s.channelName == channelName) endInput(s);
    v.erase(std::remove_if(v.begin(), v.end(),
            [&](const auto& s) {
                return s.peerName == peerName && s.channelName == channelName;
            }),
            v.end());
    mInputSubCount.store(v.size(), std::memory_order_relaxed);
}

void LinkAudioBridge::clearInputs() {
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    for (auto& s : mInputSubs) endInput(s);
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

uint64_t LinkAudioBridge::underrunsSinceStart(const InputSubscription& sub) const {
    if (!sub.underrunsAtStart || sub.port == CLOCKWORK_PORT_NONE) return 0;
    return clockwork_port_underruns(ports(), sub.port) - *sub.underrunsAtStart;
}

uint64_t LinkAudioBridge::troubleOf(const InputSubscription& sub) const {
    return underrunsSinceStart(sub) + sub.renderer->resyncs() + sub.renderer->warps();
}

/*
 * How much audio stands between a subscription and a gap, which is in two
 * places: what the renderer holds (peer buffers its network thread has
 * delivered and it has not yet resampled) and what the port holds (resampled,
 * waiting for the audio thread). The endpoint drains the renderer into the
 * port as fast as there is room, so the renderer alone sits near empty while
 * the stream is perfectly healthy: counted alone, a working subscription
 * reads as Dropout and the metric as 0 ms. Lock-free, so the audio
 * thread's metrics read it too.
 */
float LinkAudioBridge::bufferedSecondsOf(const InputSubscription& sub) const {
    const uint32_t rate = mSampleRate.load(std::memory_order_relaxed);
    const double portSeconds = (sub.port != CLOCKWORK_PORT_NONE && rate != 0)
        ? double(clockwork_port_readable(ports(), sub.port)) / double(rate)
        : 0.0;
    return sub.renderer->bufferedSeconds() + static_cast<float>(portSeconds);
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
        s.bufferedSeconds         = bufferedSecondsOf(sub);
        s.droppedSourceBuffers    = sub.renderer->droppedSourceBuffers();
        s.networkGapBuffers       = sub.renderer->networkGapBuffers();
        s.totalSourceBufferCalls  = sub.renderer->totalSourceBufferCalls();
        s.duplicateCountCalls     = sub.renderer->duplicateCountCalls();
        s.latencySeconds          = sub.renderer->latencySeconds();
        s.underruns               = underrunsSinceStart(sub);
        s.resyncs                 = sub.renderer->resyncs();
        s.warps                   = sub.renderer->warps();
        s.driftPpm                = sub.renderer->lastDriftPpm();

        // A full buffer is not a stream that is being heard: a dropout is
        // also one that has gapped, jumped or bent in the last second. The
        // stream's second, on Link's clock as the reads are dated, not the
        // machine's: the same on a freewheel clock as at a device's pace.
        constexpr float   kMinHealthyBufferSeconds = 0.005f;
        constexpr int64_t kTroubleHoldMicros = 1'000'000;
        const int64_t sinceTrouble = static_cast<int64_t>(mNextReadMicros.load(std::memory_order_relaxed))
                                   - static_cast<int64_t>(sub.troubleAtMicros);
        const bool troubledLately = sub.troubleSeen > 0 && sinceTrouble < kTroubleHoldMicros;
        const bool everReceived = sub.renderer->everReceived();
        if (!everReceived) {
            s.state = link_audio::ConnectionState::Connecting;
        } else if (s.bufferedSeconds < kMinHealthyBufferSeconds || troubledLately) {
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

void LinkAudioBridge::setAudioFormat(ClockworkCore* core, uint32_t sampleRate, uint32_t allocatedInputChannels) {
    mSampleRate.store(sampleRate, std::memory_order_relaxed);
    mInputChannels.store(allocatedInputChannels, std::memory_order_relaxed);
    mCore.store(core, std::memory_order_release);
    moveToLanes(clockwork_link_audio_lane_base(core), clockwork_link_audio_lanes(core));
    if (sampleRate != 0 && mInputSubCount.load(std::memory_order_relaxed) != 0)
        startEndpoint();
    clockwork::bump(mEndpointWake);
}

// A wider device moves the Link Audio lanes up, and every subscription moves
// with them, keeping its place among them: left where it was, it would sit on
// the device's own channels. A subscription the lanes no longer hold is
// dropped, and says so.
void LinkAudioBridge::moveToLanes(uint32_t base, uint32_t lanes) {
    std::lock_guard<std::mutex> lk(mInputSubMutex);
    const uint32_t from = mLaneBase;
    mLaneBase = base;
    mLanes    = lanes;
    if (base == from) return;
    auto& v = mInputSubs;
    for (auto& s : v) {
        const uint32_t offset = s.busIdx - from;
        const uint32_t to     = base + offset;
        if (offset + 2 <= lanes && s.port != CLOCKWORK_PORT_NONE) {
            ClockworkCore* const core = mCore.load(std::memory_order_acquire);
            clockwork_port_bus_detach(core, s.port);
            if (clockwork_port_bus_attach(core, s.port, to)) { s.busIdx = to; continue; }
        }
        clockwork_log(core(), "[link-audio] %s/%s dropped: the Link Audio lanes moved and "
                "have no room for it", s.peerName.c_str(), s.channelName.c_str());
        endInput(s);
    }
    v.erase(std::remove_if(v.begin(), v.end(),
            [](const auto& s) { return s.port == CLOCKWORK_PORT_NONE; }), v.end());
    mInputSubCount.store(v.size(), std::memory_order_relaxed);
}

// The port first, then the generation, released: an audio thread that sees
// the new generation sees the new port.
void LinkAudioBridge::dateReadsOf(uint32_t busIdx, ClockworkPort port) {
    if (busIdx < mLaneBase || (busIdx - mLaneBase) / 2 >= mReadDates.size()) return;
    auto& d = mReadDates[(busIdx - mLaneBase) / 2];
    d.port.store(port, std::memory_order_relaxed);
    d.generation.fetch_add(1, std::memory_order_release);
}

// By port, not by pair: a subscription leaving as the lanes move still names
// the pair it had.
void LinkAudioBridge::forgetReadDates(ClockworkPort port) {
    for (auto& d : mReadDates) {
        ClockworkPort expected = port;
        if (d.port.compare_exchange_strong(expected, CLOCKWORK_PORT_NONE, std::memory_order_relaxed))
            d.generation.fetch_add(1, std::memory_order_release);
    }
}

// A sequence lock: one writer, this; an odd count means it is mid-write.
void LinkAudioBridge::dateReads(uint64_t nextReadHostMicros) {
    if (nextReadHostMicros == 0) return;   // no Link clock: nothing to date by
    mDatedBlocks.fetch_add(1, std::memory_order_relaxed);
    mNextReadMicros.store(nextReadHostMicros, std::memory_order_relaxed);
    for (auto& d : mReadDates) {
        const uint32_t generation = d.generation.load(std::memory_order_acquire);
        const ClockworkPort port  = d.port.load(std::memory_order_relaxed);
        if (port == CLOCKWORK_PORT_NONE) continue;
        const uint32_t read = clockwork_port_read_position(ports(), port);
        const uint32_t seq  = d.seq.load(std::memory_order_relaxed);
        d.seq.store(seq + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        d.datedGeneration.store(generation, std::memory_order_relaxed);
        d.hostMicros.store(nextReadHostMicros, std::memory_order_relaxed);
        d.readPosition.store(read, std::memory_order_relaxed);
        d.seq.store(seq + 2, std::memory_order_release);
    }

    // What the endpoint asked to be woken for: this block, or room for a
    // chunk in a port it found full.
    if (mEndpointRequests.answer([this](uint32_t pair) {
            if (pair >= mReadDates.size()) return false;
            const ClockworkPort port = mReadDates[pair].port.load(std::memory_order_relaxed);
            return port != CLOCKWORK_PORT_NONE && clockwork_port_writable(ports(), port) >= kEndpointChunkFrames;
        }))
        clockwork::bump(mEndpointWake);
}

std::optional<std::chrono::microseconds>
LinkAudioBridge::nextWriteReadAt(const InputSubscription& sub, uint32_t sampleRate) const {
    for (const auto& d : mReadDates) {
        if (d.port.load(std::memory_order_relaxed) != sub.port) continue;
        const uint32_t generation = d.generation.load(std::memory_order_acquire);
        // A writer laps a reader only by rendering a whole block in between:
        // a few tries always find a still moment.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const uint32_t before = d.seq.load(std::memory_order_acquire);
            if (before & 1u) continue;
            const uint32_t dated = d.datedGeneration.load(std::memory_order_relaxed);
            const uint64_t host  = d.hostMicros.load(std::memory_order_relaxed);
            const uint32_t read  = d.readPosition.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (d.seq.load(std::memory_order_relaxed) != before) continue;
            if (dated != generation || host == 0) return std::nullopt;
            // Frames between the dated read and the next write, wrapping as
            // the positions do.
            const uint32_t ahead = clockwork_port_write_position(ports(), sub.port) - read;
            return std::chrono::microseconds(static_cast<int64_t>(host))
                 + std::chrono::microseconds(static_cast<int64_t>(
                       std::llround(1e6 * double(ahead) / double(sampleRate))));
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void LinkAudioBridge::startEndpoint() {
    if (mEndpointRun.exchange(true, std::memory_order_acq_rel)) return;
    if (mEndpointByHand.load(std::memory_order_relaxed)) return;   // runEndpoint runs it
    mEndpointThread = std::jthread([this](const std::stop_token& stop) { endpointLoop(stop); });
}

void LinkAudioBridge::setEndpointByHand(bool byHand) {
    stopEndpoint();
    mEndpointByHand.store(byHand, std::memory_order_relaxed);
    if (mSampleRate.load(std::memory_order_relaxed) != 0 && mInputSubCount.load(std::memory_order_relaxed) != 0)
        startEndpoint();
}

// A pass that asks to run again at once has more to do, and one that would
// sleep has nothing until something changes. The bound is a fault's: the
// passes there is work for are a few a subscription.
void LinkAudioBridge::runEndpoint() {
    if (!mEndpointByHand.load(std::memory_order_relaxed)) return;
    constexpr int kMostPasses = 4096;
    for (int pass = 0; pass < kMostPasses; ++pass)
        if (endpointPass().kind != clockwork::NextPass::Kind::Now) return;
    clockwork_log(core(), "[link-audio] the endpoint still had work after %d passes run by hand", kMostPasses);
}

void LinkAudioBridge::stopEndpoint() {
    if (!mEndpointRun.exchange(false, std::memory_order_acq_rel)) return;
    mEndpointThread.request_stop();
    if (mEndpointThread.joinable()) mEndpointThread.join();
    mEndpointRealtime.store(-1, std::memory_order_release);
}

/*
 * Fill every subscription's port with as much as it has room for.
 *
 * THE HOST TIME IS WHEN THE FRAMES BEING WRITTEN WILL BE READ, as the audio
 * thread dated it (dateReads), not now: what is already queued is read first.
 * The renderer turns that into a beat range and reads the peer's buffers,
 * which carry beat ranges of their own — which is why running ahead of the
 * audio thread is safe. A tempo change moves time-per-beat for everyone at
 * once, so queued audio stays where it belongs on the timeline.
 *
 * Short reads are NOT padded. A renderer that had less than was asked for
 * means the peer's queue is dry, and producing zeros would fill the ring with
 * silence the port could not tell from audio — the underrun would vanish from
 * the one counter that reports it. Offering only what is real lets the ring
 * run down and the audio thread read silence, counted, as the substrate
 * intends.
 */
void LinkAudioBridge::endpointLoop(const std::stop_token& stop) {
    clockwork::name_this_thread("cw-link-input");

    // Scheduled as the audio thread is: this feeds it, and from an ordinary
    // thread's place in the queue it runs tens of milliseconds late on a busy
    // machine while the ring runs dry. Asked once, at this worker's own pace
    // (a chunk at the sample rate), and answered in the log and through
    // endpointRealtime(); the engine can only ask.
    {
        const uint32_t sr = mSampleRate.load(std::memory_order_relaxed);
        const clockwork::RealtimeResult rt = clockwork::elevateCurrentThreadToRealtime(
            sr != 0 ? double(kEndpointChunkFrames) / double(sr) : 0.0);
        clockwork_log(core(), "[link-audio] endpoint worker realtime: status=%d policy=%d prio=%d err=%d",
                      static_cast<int>(rt.status), rt.policy, rt.priority, rt.error);
        mEndpointRealtime.store(static_cast<int>(rt.status), std::memory_order_release);
    }

    clockwork::run_worker(mEndpointWake, stop, [this] { return endpointPass(); });
}

// One pass over every subscription. When nothing renders, either the peers
// are quiet, and the worker asks the renderer to wake it on the next buffer,
// or only the audio thread can say when there is more to do, and the worker
// asks the audio thread. A subscription waiting on the audio thread withdraws
// its arrival ask, because buffers keep arriving while its port is full.
clockwork::NextPass LinkAudioBridge::endpointPass() {
    constexpr double kQuantum = 4.0;
    const uint32_t sr = mSampleRate.load(std::memory_order_relaxed);
    if (sr == 0) return clockwork::NextPass::woken();   // setAudioFormat wakes it

    bool produced = false, fresh = false;
    uint32_t wants = 0, fullPairs = 0;
    {
        // A blocking lock, not a try_lock: this is not the audio thread,
        // and skipping a pass on contention would starve the ring for no
        // benefit.
        std::lock_guard<std::mutex> lk(mInputSubMutex);
        if (!mInputSubs.empty()) {
            auto sessionState = mLink.captureAppSessionState();
            for (auto& sub : mInputSubs) {
                if (sub.port == CLOCKWORK_PORT_NONE || !sub.renderer) continue;
                if (const uint64_t trouble = troubleOf(sub); trouble > sub.troubleSeen) {
                    sub.troubleSeen     = trouble;
                    sub.troubleAtMicros = mNextReadMicros.load(std::memory_order_relaxed);
                }

                uint32_t room = clockwork_port_writable(ports(), sub.port);
                if (room == 0) {
                    sub.renderer->withdrawArrival();
                    wants |= clockwork::EndpointRequests::kRoom;
                    fullPairs |= 1u << ((sub.busIdx - mLaneBase) / 2);
                    continue;
                }
                if (room > kEndpointChunkFrames) room = kEndpointChunkFrames;

                const auto hostTime = nextWriteReadAt(sub, sr);
                if (!hostTime) {   // not read yet: no date to write for
                    sub.renderer->withdrawArrival();
                    wants |= clockwork::EndpointRequests::kBlock;
                    continue;
                }

                const size_t filled = sub.renderer->receive(
                    mScratchL, mScratchR, room, sessionState,
                    static_cast<double>(sr), *hostTime, kQuantum);
                if (filled == 0) {
                    if (sub.renderer->waitsForTime()) {
                        sub.renderer->withdrawArrival();
                        wants |= clockwork::EndpointRequests::kBlock;
                    } else {
                        fresh |= sub.renderer->askArrival();
                    }
                    continue;
                }

                for (size_t i = 0; i < filled; ++i) {
                    mScratchInterleaved[2 * i]     = static_cast<float>(mScratchL[i]);
                    mScratchInterleaved[2 * i + 1] = static_cast<float>(mScratchR[i]);
                }
                clockwork_port_produce(ports(), sub.port, mScratchInterleaved,
                                 static_cast<uint32_t>(filled));
                if (!sub.underrunsAtStart)
                    sub.underrunsAtStart = clockwork_port_underruns(ports(), sub.port);
                produced = true;
            }
        }
    }
    if (produced) return clockwork::NextPass::now();
    // Make both asks before using either answer: a new ask of either kind
    // needs one more look.
    const bool asked = mEndpointRequests.ask(wants, fullPairs);
    return asked || fresh ? clockwork::NextPass::now() : clockwork::NextPass::woken();
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
        for (auto& s : mInputSubs) endInput(s);
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

bool LinkAudioBridge::tryReadInputHealth(link_audio::InputHealth& out) const {
    std::unique_lock<std::mutex> lk(mInputSubMutex, std::try_to_lock);
    if (!lk.owns_lock()) return false;
    uint32_t inCh = 0, rate = 0;
    int32_t  drift = 0;
    float    bufMs = 0.0f;
    uint64_t underruns = mRetiredUnderruns;
    for (const auto& sub : mInputSubs) {
        inCh += sub.renderer->lastNumChannels();
        rate  = sub.renderer->lastSampleRate();
        drift = sub.renderer->lastDriftPpm();
        bufMs = std::max(bufMs, bufferedSecondsOf(sub) * 1000.0f);
        underruns += underrunsSinceStart(sub);
    }
    out.inChannels = inCh;
    out.streamRate = rate;
    out.driftPpm   = drift;
    out.bufferedMs = static_cast<uint32_t>(bufMs);
    out.underruns  = underruns;
    return true;
}

#endif  // CLOCKWORK_LINK_AUDIO
