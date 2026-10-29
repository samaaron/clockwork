// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// One well-formed message of each kind the Link wire target reads, written
// with upstream's own encoders, as the seeds the fuzzer starts from. Each
// file leads with the byte that picks the socket (fuzz_link_wire.cpp).
//
//   link_wire_seeds <dir>
#include <ableton/discovery/Payload.hpp>
#include <ableton/discovery/v1/Messages.hpp>
#include <ableton/link/NodeId.hpp>
#include <ableton/link/NodeState.hpp>
#include <ableton/link/PayloadEntries.hpp>
#include <ableton/link/SessionId.hpp>
#include <ableton/link/v1/Messages.hpp>
#include <ableton/link_audio/AudioBuffer.hpp>
#include <ableton/link_audio/ChannelRequests.hpp>
#include <ableton/link_audio/v1/Messages.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace ableton;
using Micros = std::chrono::microseconds;

bool write(const std::string& dir, const char* name, uint8_t socket, const std::vector<uint8_t>& bytes) {
    std::ofstream out(dir + "/" + name, std::ios::binary);
    if (!out) return false;
    out.put(static_cast<char>(socket));
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

link::NodeId id(uint8_t fill) {
    link::NodeId n{};
    for (auto& b : n) b = fill;
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: link_wire_seeds <dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    bool ok = true;

    // The discovery socket.
    {
        link::NodeState state{};
        state.sessionId = id(0x11);
        state.timeline = link::Timeline{link::Tempo{120.0}, link::Beats{0.0}, Micros{0}};
        state.startStopState = link::StartStopState{true, link::Beats{4.0}, Micros{1000}};
        std::vector<uint8_t> buf(discovery::v1::kMaxMessageSize);
        auto end = discovery::v1::aliveMessage(id(0x22), 5, toPayload(state), buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "discovery-alive", 0, buf);
        buf.assign(discovery::v1::kMaxMessageSize, 0);
        end = discovery::v1::responseMessage(id(0x22), 5, toPayload(state), buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "discovery-response", 0, buf);
        buf.assign(discovery::v1::kMaxMessageSize, 0);
        end = discovery::v1::byeByeMessage(id(0x22), buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "discovery-byebye", 0, buf);
    }

    // The measurement socket.
    {
        std::vector<uint8_t> buf(link::v1::kMaxMessageSize);
        auto end = link::v1::pingMessage(
            discovery::makePayload(link::HostTime{Micros{123456}}, link::PrevGHostTime{Micros{654321}}),
            buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "measurement-ping", 1, buf);
        buf.assign(link::v1::kMaxMessageSize, 0);
        end = link::v1::pongMessage(
            discovery::makePayload(link::SessionMembership{id(0x11)}, link::GHostTime{Micros{1}},
                                   link::PrevGHostTime{Micros{2}}, link::HostTime{Micros{3}}),
            buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "measurement-pong", 1, buf);
    }

    // The Link Audio socket.
    {
        std::vector<uint8_t> buf(link_audio::v1::kMaxMessageSize);
        const link_audio::ChannelRequest request{id(0x22), id(0x33)};
        auto end = link_audio::v1::detail::encodeMessage(id(0x22), 5, link_audio::v1::kChannelRequest,
                                                         toPayload(request), buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "audio-channel-request", 2, buf);

        buf.assign(link_audio::v1::kMaxMessageSize, 0);
        const link_audio::ChannelStopRequest stop{id(0x22), id(0x33)};
        end = link_audio::v1::detail::encodeMessage(id(0x22), 0, link_audio::v1::kStopChannelRequest,
                                                    toPayload(stop), buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "audio-channel-stop", 2, buf);

        buf.assign(link_audio::v1::kMaxMessageSize, 0);
        end = link_audio::v1::detail::encodeMessage(id(0x22), 0, link_audio::v1::kPong,
                                                    discovery::makePayload(link::HostTime{Micros{99}}),
                                                    buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "audio-pong", 2, buf);

        // Sixty-four stereo frames of a ramp, in two chunks.
        link_audio::AudioBuffer audio{};
        audio.channelId = id(0x33);
        audio.sessionId = id(0x11);
        audio.chunks = {link_audio::AudioBuffer::Chunk{7, 32, link::Beats{0.0}, link::Tempo{120.0}},
                        link_audio::AudioBuffer::Chunk{8, 32, link::Beats{0.5}, link::Tempo{120.0}}};
        audio.codec = link_audio::Codec::kPCM_i16;
        audio.sampleRate = 48000;
        audio.numChannels = 2;
        audio.numBytes = 64 * 2 * 2;
        for (uint32_t i = 0; i < audio.numBytes; ++i) audio.bytes[i] = static_cast<uint8_t>(i);
        buf.assign(link_audio::v1::kMaxMessageSize, 0);
        end = link_audio::v1::audioBufferMessage(id(0x22), audio, buf.begin());
        buf.resize(static_cast<size_t>(end - buf.begin()));
        ok &= write(dir, "audio-buffer", 2, buf);
    }

    if (!ok) {
        std::fprintf(stderr, "link_wire_seeds: could not write into %s\n", dir.c_str());
        return 1;
    }
    return 0;
}
