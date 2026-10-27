// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_oscpack.cpp — pins the changes clockwork made to its copy of oscpack
 * (src/vendor/oscpack/CLOCKWORK-CHANGES.md). A malformed packet must end in an
 * osc::Exception: never in a read past the packet, and never in a loop that
 * does not end.
 */
#include <catch2/catch_test_macros.hpp>

#include <osc/OscReceivedElements.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

// A bundle of 20 bytes: the address, a time tag, then `sizeField` as the
// first element's size, big-endian, with nothing after it.
std::vector<uint32_t> bundleWithElementSize(uint32_t sizeField) {
    std::vector<uint32_t> words(5, 0);
    std::memcpy(words.data(), "#bundle\0", 8);
    const uint32_t be = (sizeField << 24) | ((sizeField & 0xFF00u) << 8)
                      | ((sizeField >> 8) & 0xFF00u) | (sizeField >> 24);
    words[4] = be;
    return words;
}

osc::ReceivedPacket packetOf(const std::vector<uint32_t>& words) {
    return osc::ReceivedPacket(reinterpret_cast<const char*>(words.data()),
                               static_cast<osc::osc_bundle_element_size_t>(words.size() * sizeof(uint32_t)));
}

}  // namespace

TEST_CASE("oscpack: a bundle element larger than what is left of the bundle is refused",
          "[osc][oscpack]") {
    // Change 4: 1.1.0 added the element size to the pointer in 32-bit unsigned
    // arithmetic and compared the pointer afterwards, so a size near 2^32
    // wrapped the sum, the pointer stayed where it was, and the loop never
    // ended. A size of 32 with nothing after it is the plain case.
    for (const uint32_t size : { 0xFFFFFFFCu, 0xFFFFFFF8u, 0x80000000u, 32u }) {
        const auto words = bundleWithElementSize(size);
        const auto packet = packetOf(words);
        REQUIRE_THROWS_AS([&] { osc::ReceivedBundle bundle(packet); (void)bundle; }(),
                          osc::MalformedBundleException);
    }
}

TEST_CASE("oscpack: a bundle whose one element fits exactly is read", "[osc][oscpack]") {
    const auto words = bundleWithElementSize(0);
    const auto packet = packetOf(words);
    const osc::ReceivedBundle bundle(packet);
    CHECK(bundle.ElementCount() == 1);
}
