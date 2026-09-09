// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * OscTestUtils.cpp — see OscTestUtils.h.
 */
#include "OscTestUtils.h"

#include <vector>

#include <cstring>

namespace osc_test {
namespace {

Packet streamToPacket(osc::OutboundPacketStream& s) {
    Packet p;
    p.data.assign(s.Data(), s.Data() + s.Size());
    return p;
}

size_t alignedUp(size_t x) { return (x + 3) & ~size_t(3); }

uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
         | (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

uint64_t readU64BE(const uint8_t* p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x = (x << 8) | p[i];
    return x;
}

float readF32BE(const uint8_t* p) {
    const uint32_t bits = readU32BE(p);
    float f; std::memcpy(&f, &bits, 4); return f;
}

double readF64BE(const uint8_t* p) {
    const uint64_t bits = readU64BE(p);
    double f; std::memcpy(&f, &bits, 8); return f;
}

struct ArgRef {
    char           tag   = 0;
    const uint8_t* p     = nullptr;   // start of this argument's bytes
    size_t         avail = 0;         // bytes from p to the end of the packet
};

// Locate argument `index`. tag == 0 on malformed input or out of range.
ArgRef locateArg(const std::vector<uint8_t>& raw, int index) {
    ArgRef none;
    const uint8_t* d = raw.data();
    const size_t   n = raw.size();
    size_t p = 0;
    while (p < n && d[p]) ++p;                     // address
    if (p >= n) return none;
    p = alignedUp(p + 1);
    if (p >= n || d[p] != ',') return none;        // type tags
    const size_t tagStart = p;
    while (p < n && d[p]) ++p;
    if (p >= n) return none;
    const std::string tags(reinterpret_cast<const char*>(d + tagStart), p - tagStart);
    p = alignedUp(p + 1);
    for (size_t t = 1; t < tags.size(); ++t) {
        const char tag = tags[t];
        size_t sz = 0;
        switch (tag) {
            case 'i': case 'f':            sz = 4; break;
            case 'h': case 'd': case 't':  sz = 8; break;
            case 's': case 'S': {
                size_t q = p;
                while (q < n && d[q]) ++q;
                if (q >= n) return none;
                sz = alignedUp(q + 1) - p;
                break;
            }
            case 'b': {
                if (p + 4 > n) return none;
                sz = 4 + alignedUp(readU32BE(d + p));
                break;
            }
            case 'T': case 'F': case 'N': case 'I': sz = 0; break;
            default: return none;
        }
        if (p + sz > n) return none;
        if (static_cast<int>(t) - 1 == index) return ArgRef{tag, d + p, n - p};
        p += sz;
    }
    return none;
}

} // namespace

Packet message(const char* address) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, int32_t a) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << a << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, int32_t a, int32_t b) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << a << b << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, int32_t a, int32_t b, int32_t c) {
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(address) << a << b << c << osc::EndMessage;
    return streamToPacket(s);
}

Packet message(const char* address, const char* s) {
    char buf[256];
    osc::OutboundPacketStream stream(buf, sizeof(buf));
    stream << osc::BeginMessage(address) << s << osc::EndMessage;
    return streamToPacket(stream);
}

Packet messageWithBlob(const char* address, const void* blobData, size_t blobSize) {
    // memcpy(dst, NULL, 0) is UB by the C standard even at n == 0, and
    // std::vector<T>::data() may return nullptr when empty — so hand oscpack a
    // valid (never-read) source instead.
    static const char kEmptyBlobSentinel = 0;
    const void* src = blobData ? blobData : &kEmptyBlobSentinel;

    // Sized to the blob rather than to a fixed 64 KB. A blob is the one
    // argument whose size is not bounded by anything in the message format, so
    // a fixed staging buffer here caps what the SUITE can test rather than
    // what clockwork can carry — and it did: the first case to send a 64 KB
    // blob failed with "out of buffer memory" from oscpack, which reads like a
    // clockwork limit and is not one.
    std::vector<char> buf(blobSize + 1024);   // address, type tag, size word, padding
    osc::OutboundPacketStream s(buf.data(), buf.size());
    s << osc::BeginMessage(address)
      << osc::Blob(src, static_cast<osc::osc_bundle_element_size_t>(blobSize))
      << osc::EndMessage;
    return streamToPacket(s);
}

Builder::Builder() = default;

osc::OutboundPacketStream& Builder::begin(const char* address) {
    mStream = std::make_unique<osc::OutboundPacketStream>(mBuf, kBufSize);
    *mStream << osc::BeginMessage(address);
    return *mStream;
}

Packet Builder::end() {
    *mStream << osc::EndMessage;
    return streamToPacket(*mStream);
}

Packet bundle(uint64_t timetag, const std::vector<Packet>& elements) {
    // Built by hand rather than through oscpack so a test can put ANY bytes in
    // an element, including a deliberately malformed one.
    Packet p;
    const char* tag = "#bundle";
    p.data.insert(p.data.end(), tag, tag + 8);              // "#bundle" + NUL
    for (int i = 7; i >= 0; --i)
        p.data.push_back(static_cast<uint8_t>(timetag >> (i * 8)));
    for (const auto& e : elements) {
        const uint32_t n = e.size();
        for (int i = 3; i >= 0; --i)
            p.data.push_back(static_cast<uint8_t>(n >> (i * 8)));
        p.data.insert(p.data.end(), e.data.begin(), e.data.end());
    }
    return p;
}

std::string parseAddress(const uint8_t* data, uint32_t size) {
    if (!data || size == 0) return {};
    const char* str = reinterpret_cast<const char*>(data);
    const size_t len = ::strnlen(str, size);
    return std::string(str, len);
}

int32_t ParsedReply::argInt(int index) const {
    const ArgRef a = locateArg(raw, index);
    if (a.tag == 'i' && a.avail >= 4) return static_cast<int32_t>(readU32BE(a.p));
    if (a.tag == 'f' && a.avail >= 4) return static_cast<int32_t>(readF32BE(a.p));
    return 0;
}

int64_t ParsedReply::argInt64(int index) const {
    const ArgRef a = locateArg(raw, index);
    if ((a.tag == 'h' || a.tag == 't') && a.avail >= 8)
        return static_cast<int64_t>(readU64BE(a.p));
    if (a.tag == 'i' && a.avail >= 4) return static_cast<int32_t>(readU32BE(a.p));
    return 0;
}

float ParsedReply::argFloat(int index) const {
    const ArgRef a = locateArg(raw, index);
    if (a.tag == 'f' && a.avail >= 4) return readF32BE(a.p);
    if (a.tag == 'i' && a.avail >= 4)
        return static_cast<float>(static_cast<int32_t>(readU32BE(a.p)));
    return 0.0f;
}

double ParsedReply::argDouble(int index) const {
    const ArgRef a = locateArg(raw, index);
    if (a.tag == 'd' && a.avail >= 8) return readF64BE(a.p);
    if (a.tag == 'f' && a.avail >= 4) return readF32BE(a.p);
    if (a.tag == 'i' && a.avail >= 4)
        return static_cast<double>(static_cast<int32_t>(readU32BE(a.p)));
    return 0.0;
}

std::string ParsedReply::argString(int index) const {
    const ArgRef a = locateArg(raw, index);
    if (a.tag != 's' && a.tag != 'S') return {};
    size_t len = 0;
    while (len < a.avail && a.p[len]) ++len;
    return std::string(reinterpret_cast<const char*>(a.p), len);
}

std::vector<uint8_t> ParsedReply::argBlob(int index) const {
    const ArgRef a = locateArg(raw, index);
    if (a.tag != 'b' || a.avail < 4) return {};
    const uint32_t n = readU32BE(a.p);
    if (4 + static_cast<size_t>(n) > a.avail) return {};
    return std::vector<uint8_t>(a.p + 4, a.p + 4 + n);
}

int ParsedReply::argCount() const {
    const uint8_t* d = raw.data();
    const size_t   n = raw.size();
    size_t p = 0;
    while (p < n && d[p]) ++p;
    if (p >= n) return 0;
    p = alignedUp(p + 1);
    if (p >= n || d[p] != ',') return 0;
    const size_t tagStart = p;
    while (p < n && d[p]) ++p;
    if (p >= n) return 0;
    return static_cast<int>(p - tagStart - 1);
}

ParsedReply parseReply(const uint8_t* data, uint32_t size) {
    ParsedReply r;
    r.raw.assign(data, data + size);
    r.address = parseAddress(data, size);
    return r;
}

} // namespace osc_test
