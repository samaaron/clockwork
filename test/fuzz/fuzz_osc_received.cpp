// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// oscpack's reader, as the engine drives it on every message a client sends:
// a packet, then a bundle's elements to any depth, then each argument read as
// the type it claims to be. A malformed packet must end in osc::Exception and
// nowhere else; the sanitizers catch any read past the buffer on the way.

#include <osc/OscReceivedElements.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

volatile uint64_t gSink;

void readMessage(const osc::ReceivedMessage& m) {
    gSink += std::strlen(m.AddressPattern());
    for (auto a = m.ArgumentsBegin(); a != m.ArgumentsEnd(); ++a) {
        if (a->IsInt32())        gSink += static_cast<uint64_t>(a->AsInt32());
        else if (a->IsInt64())   gSink += static_cast<uint64_t>(a->AsInt64());
        else if (a->IsFloat())   gSink += static_cast<uint64_t>(a->AsFloat() != 0.f);
        else if (a->IsDouble())  gSink += static_cast<uint64_t>(a->AsDouble() != 0.0);
        else if (a->IsTimeTag()) gSink += a->AsTimeTag();
        else if (a->IsChar())    gSink += static_cast<uint64_t>(a->AsChar());
        else if (a->IsRgbaColor())  gSink += a->AsRgbaColor();
        else if (a->IsMidiMessage()) gSink += a->AsMidiMessage();
        else if (a->IsString())  gSink += std::strlen(a->AsString());
        else if (a->IsSymbol())  gSink += std::strlen(a->AsSymbol());
        else if (a->IsBlob()) {
            const void* data = nullptr;
            osc::osc_bundle_element_size_t size = 0;
            a->AsBlob(data, size);
            if (size > 0) gSink += static_cast<const uint8_t*>(data)[size - 1];
        }
    }
}

void readBundle(const osc::ReceivedBundle& b, int depth) {
    gSink += b.TimeTag();
    if (depth > 64) return;
    for (auto e = b.ElementsBegin(); e != b.ElementsEnd(); ++e) {
        if (e->IsBundle()) readBundle(osc::ReceivedBundle(*e), depth + 1);
        else readMessage(osc::ReceivedMessage(*e));
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // oscpack wants its packet 4-byte aligned, as a datagram buffer is.
    std::vector<uint32_t> aligned((size + 3) / 4);
    if (size) std::memcpy(aligned.data(), data, size);
    try {
        osc::ReceivedPacket p(reinterpret_cast<const char*>(aligned.data()),
                              static_cast<osc::osc_bundle_element_size_t>(size));
        if (p.IsBundle()) readBundle(osc::ReceivedBundle(p), 0);
        else readMessage(osc::ReceivedMessage(p));
    } catch (const osc::Exception&) {
    }
    return 0;
}
