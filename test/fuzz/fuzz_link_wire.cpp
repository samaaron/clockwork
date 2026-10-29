// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Ableton Link's wire, as a stranger on the network segment reaches it. Three
// sockets take datagrams from any peer once Link is on, and each has one
// parser behind it:
//
//   the discovery socket: session messages (alive, response, bye-bye), parsed
//     by discovery::v1::parseMessageHeader and NodeState::fromPayload
//   the measurement socket: ping and pong, parsed by link::v1::parseMessageHeader
//     and the payload entries the responder and the measurement read
//   the Link Audio socket: announcements, byes, pongs, channel requests and
//     audio buffers, parsed by link_audio::v1::parseMessageHeader and each
//     message's own reader, with an audio buffer decoded to samples as a
//     source does
//
// The first byte picks the socket; the rest is the datagram, held to that
// socket's buffer as the OS holds it. Upstream's contract is that a bad
// message is a std::runtime_error, which every receiver catches and logs;
// that is caught here too. Anything else — a crash, a read out of range, an
// exception of another kind — is a finding.
#include <ableton/discovery/Payload.hpp>
#include <ableton/discovery/v1/Messages.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/link/PayloadEntries.hpp>
#include <ableton/link/SessionId.hpp>
#include <ableton/link/v1/Messages.hpp>
#include <ableton/link_audio/AudioBuffer.hpp>
#include <ableton/link_audio/Buffer.hpp>
#include <ableton/link_audio/ChannelAnnouncements.hpp>
#include <ableton/link_audio/ChannelRequests.hpp>
#include <ableton/link_audio/PCMCodec.hpp>
#include <ableton/link_audio/PeerAnnouncement.hpp>
#include <ableton/link_audio/v1/Messages.hpp>
#include <ableton/util/Injected.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

using namespace ableton;

volatile uint64_t gSink;

// What a source does with a decoded buffer: read every sample of every frame
// the decoder says it holds.
struct TakeSamples {
    void operator()(link_audio::BufferCallbackHandle<link_audio::Buffer<int16_t>> h) {
        const uint32_t n = h.mBuffer.mNumFrames * h.mBuffer.mNumChannels;
        for (uint32_t i = 0; i < n; ++i) gSink += static_cast<uint64_t>(h.mpSamples[i]);
    }
};

void discoverySocket(const uint8_t* b, const uint8_t* e) {
    auto r = discovery::v1::parseMessageHeader<link::NodeId>(b, e);
    const auto& header = r.first;
    gSink += header.ttl;
    switch (header.messageType) {
        case discovery::v1::kAlive:
        case discovery::v1::kResponse: {
            auto state = link::NodeState::fromPayload(header.ident, r.second, e);
            // A peer may send a tempo of zero microseconds per beat, which is an
            // infinite bpm until Link clamps it on adoption: looked at, not cast.
            gSink += std::isfinite(state.timeline.tempo.bpm()) ? 1u : 2u;
            gSink += static_cast<uint64_t>(state.timeline.timeOrigin.count());
            break;
        }
        default: break;
    }
}

void measurementSocket(const uint8_t* b, const uint8_t* e) {
    auto r = link::v1::parseMessageHeader(b, e);
    const auto& header = r.first;
    if (header.messageType == link::v1::kPing) {
        discovery::parsePayload<link::HostTime, link::PrevGHostTime>(
            r.second, e,
            [](link::HostTime ht) { gSink += static_cast<uint64_t>(ht.time.count()); },
            [](link::PrevGHostTime gt) { gSink += static_cast<uint64_t>(gt.time.count()); });
    } else if (header.messageType == link::v1::kPong) {
        discovery::parsePayload<link::SessionMembership, link::GHostTime, link::PrevGHostTime, link::HostTime>(
            r.second, e,
            [](link::SessionMembership sm) { gSink += sm.sessionId[0]; },
            [](link::GHostTime gt) { gSink += static_cast<uint64_t>(gt.time.count()); },
            [](link::PrevGHostTime gt) { gSink += static_cast<uint64_t>(gt.time.count()); },
            [](link::HostTime ht) { gSink += static_cast<uint64_t>(ht.time.count()); });
    }
}

void audioSocket(const uint8_t* b, const uint8_t* e) {
    auto r = link_audio::v1::parseMessageHeader(b, e);
    const auto& header = r.first;
    gSink += header.ttl;
    switch (header.messageType) {
        case link_audio::v1::kPeerAnnouncement: {
            auto a = link_audio::PeerAnnouncement::fromPayload(header.ident, r.second, e);
            gSink += a.channels.channels.size();
            discovery::parsePayload<link::HostTime>(
                r.second, e, [](link::HostTime ht) { gSink += static_cast<uint64_t>(ht.time.count()); });
            break;
        }
        case link_audio::v1::kChannelByes: {
            discovery::parsePayload<link_audio::ChannelByes>(
                r.second, e, [](const link_audio::ChannelByes& byes) { gSink += byes.byes.size(); });
            break;
        }
        case link_audio::v1::kPong: {
            discovery::parsePayload<link::HostTime>(
                r.second, e, [](link::HostTime ht) { gSink += static_cast<uint64_t>(ht.time.count()); });
            break;
        }
        case link_audio::v1::kChannelRequest: {
            auto q = link_audio::ChannelRequest::fromPayload(header.ident, r.second, e);
            gSink += q.channelId[0];
            break;
        }
        case link_audio::v1::kStopChannelRequest: {
            auto q = link_audio::ChannelStopRequest::fromPayload(header.ident, r.second, e);
            gSink += q.channelId[0];
            break;
        }
        case link_audio::v1::kAudioBuffer: {
            static link_audio::AudioBuffer buffer{};
            link_audio::AudioBuffer::fromNetworkByteStream(buffer, r.second, e);
            // Decoded as a source decodes it: the same cache size.
            static link_audio::PCMDecoder<int16_t, TakeSamples> decoder(util::injectVal(TakeSamples{}), 4096);
            decoder(buffer);
            break;
        }
        default: break;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const uint8_t which = data[0];
    const uint8_t* b = data + 1;
    size_t n = size - 1;
    try {
        switch (which % 3) {
            case 0:
                n = std::min(n, discovery::v1::kMaxMessageSize);
                discoverySocket(b, b + n);
                break;
            case 1:
                n = std::min(n, link::v1::kMaxMessageSize);
                measurementSocket(b, b + n);
                break;
            default:
                n = std::min(n, link_audio::v1::kMaxMessageSize);
                audioSocket(b, b + n);
                break;
        }
    } catch (const std::runtime_error&) {
        // A malformed message, refused as upstream refuses it.
    }
    return 0;
}
