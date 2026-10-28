// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The standalone host's own OSC reader (src/host/osc_reader.h) promises that
// a malformed packet leaves ok() false and every read fails rather than
// reading out of range. Read whatever the type tags claim, to the end.

#include "host/osc_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
volatile uint64_t gSink;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    clockwork_host::OscReader r(data, size);
    if (!r.ok()) return 0;
    gSink += std::strlen(r.address());
    for (int guard = 0; guard < 4096; ++guard) {
        int32_t i; int64_t h; float f; double d; const char* s; const uint8_t* b; uint32_t n;
        bool read = false;
        switch (r.peekType()) {
            case 0:   return 0;
            case 'i': read = r.readInt32(i);  if (read) gSink += static_cast<uint64_t>(i); break;
            case 'h': read = r.readInt64(h);  if (read) gSink += static_cast<uint64_t>(h); break;
            case 'f': read = r.readFloat(f);  if (read) gSink += f != 0.f; break;
            case 'd': read = r.readDouble(d); if (read) gSink += d != 0.0; break;
            case 's': read = r.readString(s); if (read) gSink += std::strlen(s); break;
            case 'b': read = r.readBlob(b, n); if (read && n) gSink += b[n - 1]; break;
            default:  read = r.skip(); break;
        }
        if (!read) return 0;
    }
    return 0;
}
