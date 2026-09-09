// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * OscTestUtils.h — OSC message builder and reply parser for clockwork tests.
 *
 * Deliberately smaller than the version this was taken from. That one also
 * carried builders for a DSP's own words — program images, definition blobs,
 * a uuid argument type — and none of those belong in a repository whose claim
 * is that it contains neither guest's vocabulary. What is left is generic OSC:
 * build a message, read one back.
 *
 * Uses the vendored oscpack (src/vendor/oscpack, MIT, Ross Bencina) for
 * encoding, and a hand-rolled reader for decoding so a malformed reply is a
 * failed assertion rather than a thrown exception.
 */
#pragma once

#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace osc_test {

// A self-contained OSC packet (owns its data).
struct Packet {
    std::vector<uint8_t> data;
    const uint8_t* ptr()  const { return data.data(); }
    uint32_t       size() const { return static_cast<uint32_t>(data.size()); }
};

Packet message(const char* address);
Packet message(const char* address, int32_t a);
Packet message(const char* address, int32_t a, int32_t b);
Packet message(const char* address, int32_t a, int32_t b, int32_t c);
Packet message(const char* address, const char* s);
Packet messageWithBlob(const char* address, const void* blobData, size_t blobSize);

// General-purpose builder: begin(), stream args, end().
class Builder {
public:
    Builder();
    osc::OutboundPacketStream& begin(const char* address);
    Packet end();
private:
    // A whole datagram inside two wrappers (/clockwork/osc/send inside
    // /clockwork/schedule) has to fit, so twice a datagram.
    static constexpr size_t kBufSize = 131072;
    char mBuf[kBufSize];
    std::unique_ptr<osc::OutboundPacketStream> mStream;
};

// Build a timestamped OSC bundle from already-encoded elements.
Packet bundle(uint64_t timetag, const std::vector<Packet>& elements);

// The address pattern of a raw OSC packet.
std::string parseAddress(const uint8_t* data, uint32_t size);

// A parsed reply. Accessors return 0/0.0/"" for a missing or wrongly-typed
// argument rather than throwing, so a test asserts on the value it expected
// instead of dying on the packet it got.
struct ParsedReply {
    std::string address;
    std::vector<uint8_t> raw;

    int32_t     argInt(int index) const;
    int64_t     argInt64(int index) const;
    float       argFloat(int index) const;
    double      argDouble(int index) const;
    std::string argString(int index) const;
    // The bytes of a blob argument; empty when absent or wrongly typed.
    std::vector<uint8_t> argBlob(int index) const;
    int         argCount() const;
};

ParsedReply parseReply(const uint8_t* data, uint32_t size);

} // namespace osc_test
