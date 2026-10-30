// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * ForwardedBundle.h — the bundle the engine forwards to its host when
 * host forwarding is on (clockwork_host_forward, lanes.h): "#bundle", the
 * moment the element is for, and one element.
 */
#pragma once

#include "OscTestUtils.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace osc_test {

struct Forwarded {
    bool        isBundle = false;
    uint64_t    timetag  = 0;
    std::string address;               // of the one element inside
    std::vector<uint8_t> element;
};

// Unwraps one forwarded bundle; `isBundle` is false for anything else.
inline Forwarded unwrapForwarded(const uint8_t* data, size_t size) {
    Forwarded out;
    if (size < 20 || std::memcmp(data, "#bundle", 8) != 0) return out;
    out.isBundle = true;
    for (int i = 0; i < 8; ++i) out.timetag = (out.timetag << 8) | data[8 + i];
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | data[16 + i];
    if (20 + n > size) return out;
    out.element.assign(data + 20, data + 20 + n);
    out.address = parseAddress(out.element.data(), n);
    return out;
}

inline bool isBundle(const uint8_t* data, size_t size) {
    return size >= 8 && std::memcmp(data, "#bundle", 8) == 0;
}

} // namespace osc_test
