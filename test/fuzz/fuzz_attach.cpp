// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The attach endpoint's two readings of bytes it did not write
// (src/shm_attach.hpp): the server's of a request, which any process of the
// user's may send, and the client's of the hello that comes back. Neither
// reads past what it was given. The secret check itself is the comms
// library's, in Rust and fuzzed there; here it is a stand-in.

#include "shm_attach.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace {

volatile uint64_t gSink;

// The stand-in admits a packet only when it is exactly this secret's auth
// packet.
constexpr std::array<uint8_t, 32> kSecret = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
};

}  // namespace

extern "C" int32_t clockwork_comms_secret_admits(const ClockworkCommsSecret*, const uint8_t* packet, uint32_t len) {
    const auto frame = shm_attach::auth_frame(kSecret.data());
    return len == shm_attach::AUTH_PACKET_LEN && std::memcmp(packet, frame.data() + 4, len) == 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // The request: the first REQUEST_LEN bytes, as the server reads exactly
    // that many before it looks.
    std::array<uint8_t, shm_attach::REQUEST_LEN> req {};
    std::memcpy(req.data(), data, std::min(size, req.size()));
    if (const auto role = shm_attach::admitted_role(req, nullptr)) gSink += *role;

    // The hello: what follows, as the client reads exactly a hello's size.
    if (size > req.size()) {
        shm_attach::hello h {};
        std::memcpy(&h, data + req.size(), std::min(size - req.size(), sizeof h));
        for (const uint32_t role : { uint32_t{shm_attach::ROLE_READER}, uint32_t{shm_attach::ROLE_PEER}, h.role })
            gSink += shm_attach::hello_matches(h, role);
    }
    return 0;
}
