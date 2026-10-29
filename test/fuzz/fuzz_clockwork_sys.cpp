// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The audio thread's own verbs (src/clockwork_sys.h): handle_clockwork_sys_osc
// answers ping, sync, echo and the clock snapshot inside the block, and
// clockwork_sys_refuse builds the /clockwork/error reply for everything else,
// echoing the frame's own address. The frame is whatever a peer wrote into a
// ring, validated by nothing before this. Both must read the frame's `len`
// bytes and no more, and every reply must decode as a message of clockwork's.

#include "clockwork_sys.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Every reply decodes as a message under clockwork's prefix.
void checkReply(const uint8_t* reply, uint32_t n) {
    if (n == 0 || n % 4 != 0) std::abort();
    osc::ReceivedMessage m(osc::ReceivedPacket(reinterpret_cast<const char*>(reply),
                                               static_cast<osc::osc_bundle_element_size_t>(n)));
    if (!clockwork_sys_claims(reply, n)) std::abort();
    if (std::strcmp(m.AddressPattern(), CLOCKWORK_SYS("error")) == 0 && m.ArgumentCount() != 3)
        std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 0xFFFF) return 0;
    // An exact-size copy: a read past `size` is a heap overflow the sanitizer sees.
    std::vector<uint8_t> frame(data, data + size);
    const auto len = static_cast<uint32_t>(frame.size());

    uint32_t refused = 0;
    clockwork_sys_refuse(frame.data(), len, "fuzz", 7, [&](const uint8_t* reply, uint32_t n) {
        ++refused;
        checkReply(reply, n);
    });
    if (refused > 1) std::abort();

    // No clock: the snapshot verb is an unknown verb and is refused.
    uint32_t answered = 0;
    const bool claimed = handle_clockwork_sys_osc(frame.data(), len, [&](const uint8_t* reply, uint32_t n) {
        ++answered;
        checkReply(reply, n);
    }, nullptr, 7);
    // A claimed frame gets at most one reply; an unclaimed one gets none.
    if (answered > 1) std::abort();
    if (!claimed && answered != 0) std::abort();
    if (claimed != clockwork_sys_claims(frame.data(), len)) std::abort();
    return 0;
}
