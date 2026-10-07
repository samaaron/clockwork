// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * OscBuilder.h — Header-only OSC message/bundle builder
 *
 * Promotes the oscpack builder pattern from test utils to a public engine API.
 * Uses C++17 fold expressions over oscpack's operator<< overloads.
 */
#pragma once

#include "osc/OscOutboundPacketStream.h"
#include "osc/OscTypes.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <initializer_list>

// Self-contained OSC packet (owns its data)
struct OscPacket {
    std::vector<uint8_t> data;
    const uint8_t* ptr()  const { return data.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(data.size()); }
};

// A packet of the size what it carries needs, built off the audio thread.
// oscpack writes into a buffer fixed before the first byte and throws past
// its end, so a reply built in a fixed buffer on the stack threw whenever a
// client's string (a port name, a device name) or an error was longer than
// the author guessed — and nothing on the thread that built it caught, so one
// message from a client ended the process. `write` builds the whole message
// (BeginMessage through EndMessage) into the stream it is given; the buffer
// starts at `firstTry` bytes and doubles until the message fits. It
// allocates, so never from the audio thread.
template <typename Write>
OscPacket oscPacketOf(size_t firstTry, Write&& write) {
    std::vector<uint8_t> buf(firstTry < 64 ? 64 : firstTry);
    for (;;) {
        try {
            osc::OutboundPacketStream s(reinterpret_cast<char*>(buf.data()), buf.size());
            write(s);
            buf.resize(s.Size());
            return OscPacket{std::move(buf)};
        } catch (const osc::OutOfBufferMemoryException&) {
            buf.assign(buf.size() * 2, 0);
        }
    }
}

class OscBuilder {
public:
    // Blob wrapper — explicitly marks binary data for OSC blob encoding
    struct Blob {
        const void* data;
        size_t size;
    };

    // Build a single OSC message with typed args
    template<typename... Args>
    static OscPacket message(const char* address, Args&&... args) {
        thread_local std::array<char, 65536> buf;
        osc::OutboundPacketStream s(buf.data(), buf.size());
        s << osc::BeginMessage(address);
        (append(s, std::forward<Args>(args)), ...);
        s << osc::EndMessage;
        return OscPacket{{
            reinterpret_cast<const uint8_t*>(s.Data()),
            reinterpret_cast<const uint8_t*>(s.Data()) + s.Size()
        }};
    }

    // Build an OSC bundle from pre-built messages
    // Constructs wire format directly: "#bundle\0" + timetag + [size+data]*
    static OscPacket bundle(uint64_t ntpTimeTag,
                            std::initializer_list<OscPacket> messages) {
        // Calculate total size
        size_t total = 8 + 8; // "#bundle\0" + timetag
        for (auto& m : messages)
            total += 4 + m.size(); // size prefix + data

        std::vector<uint8_t> result(total);
        uint8_t* p = result.data();

        // Write "#bundle" and its NUL: eight bytes, binary, not a string
        static constexpr std::array<uint8_t, 8> kBundleTag{ '#', 'b', 'u', 'n', 'd', 'l', 'e', 0 };
        std::copy(kBundleTag.begin(), kBundleTag.end(), p);
        p += 8;

        // Write timetag (big-endian uint64)
        p[0] = static_cast<uint8_t>((ntpTimeTag >> 56u) & 0xFFu);
        p[1] = static_cast<uint8_t>((ntpTimeTag >> 48u) & 0xFFu);
        p[2] = static_cast<uint8_t>((ntpTimeTag >> 40u) & 0xFFu);
        p[3] = static_cast<uint8_t>((ntpTimeTag >> 32u) & 0xFFu);
        p[4] = static_cast<uint8_t>((ntpTimeTag >> 24u) & 0xFFu);
        p[5] = static_cast<uint8_t>((ntpTimeTag >> 16u) & 0xFFu);
        p[6] = static_cast<uint8_t>((ntpTimeTag >> 8u) & 0xFFu);
        p[7] = static_cast<uint8_t>(ntpTimeTag & 0xFFu);
        p += 8;

        // Write each message with size prefix
        for (auto& m : messages) {
            uint32_t sz = m.size();
            p[0] = static_cast<uint8_t>((sz >> 24u) & 0xFFu);
            p[1] = static_cast<uint8_t>((sz >> 16u) & 0xFFu);
            p[2] = static_cast<uint8_t>((sz >> 8u) & 0xFFu);
            p[3] = static_cast<uint8_t>(sz & 0xFFu);
            p += 4;
            std::memcpy(p, m.ptr(), sz);
            p += sz;
        }

        return OscPacket{std::move(result)};
    }

private:
    // Type dispatch — explicit overloads give clear compile errors for unsupported types
    static void append(osc::OutboundPacketStream& s, int v)                { s << static_cast<osc::int32>(v); }
    static void append(osc::OutboundPacketStream& s, float v)              { s << v; }
    static void append(osc::OutboundPacketStream& s, double v)             { s << v; }
    static void append(osc::OutboundPacketStream& s, const char* v)        { s << v; }
    static void append(osc::OutboundPacketStream& s, const std::string& v) { s << v.c_str(); }
    static void append(osc::OutboundPacketStream& s, const Blob& b) {
        s << osc::Blob(b.data, static_cast<osc::osc_bundle_element_size_t>(b.size));
    }
};
