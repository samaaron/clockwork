// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_os_pages.cpp — memory straight from the system (src/os_pages.h): whole
 * chunks on a chunk, zero when mapped, and made to read zero again by giving
 * pages back rather than by writing them.
 */
#include "os_pages.h"
#include "clockwork_heap.h"
#include "ResidentMemory.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace {
constexpr size_t kChunk = CLOCKWORK_HEAP_CHUNK_BYTES;

size_t countNot(const uint8_t* p, size_t bytes, uint8_t value) {
    size_t n = 0;
    for (size_t i = 0; i < bytes; ++i) n += p[i] != value;
    return n;
}
}

TEST_CASE("pages: a mapping is whole chunks on a chunk, and reads zero", "[pages]") {
    const size_t asked = 3 * kChunk + 100;
    CHECK(clockwork::pages::whole_chunks(asked) == 4 * kChunk);
    auto* p = static_cast<uint8_t*>(clockwork::pages::map(asked));
    REQUIRE(p != nullptr);
    CHECK(reinterpret_cast<uintptr_t>(p) % kChunk == 0);
    CHECK(countNot(p, 4 * kChunk, 0) == 0);
    clockwork::pages::unmap(p, asked);
}

TEST_CASE("pages: zero makes a range read zero again and leaves its neighbours", "[pages]") {
    auto* p = static_cast<uint8_t*>(clockwork::pages::map(4 * kChunk));
    REQUIRE(p != nullptr);
    std::memset(p, 0x5A, 4 * kChunk);
    REQUIRE(clockwork::pages::zero(p + kChunk, 2 * kChunk) == clockwork::pages::Zero::Done);
    CHECK(countNot(p + kChunk, 2 * kChunk, 0) == 0);
    CHECK(countNot(p, kChunk, 0x5A) == 0);
    CHECK(countNot(p + 3 * kChunk, kChunk, 0x5A) == 0);

    // A length short of a page is the whole of its last page: the caller's
    // through the end of it.
    const size_t page = clockwork::pages::page_bytes();
    REQUIRE(page > 0);
    REQUIRE(clockwork::pages::zero(p, 100) == clockwork::pages::Zero::Done);
    CHECK(countNot(p, page, 0) == 0);
    CHECK(countNot(p + page, kChunk - page, 0x5A) == 0);

    // And the range can be written again.
    std::memset(p + kChunk, 0x33, kChunk);
    CHECK(countNot(p + kChunk, kChunk, 0x33) == 0);
    clockwork::pages::unmap(p, 4 * kChunk);
}

TEST_CASE("pages: zero gives the pages back to the system", "[pages][memory]") {
    constexpr size_t kBytes = 64u << 20;
    auto* p = static_cast<uint8_t*>(clockwork::pages::map(kBytes));
    REQUIRE(p != nullptr);
    const size_t untouched = test_memory::residentBytes();
    if (untouched == 0) SKIP("this platform does not say how much of the process is resident");
    std::memset(p, 0x77, kBytes);
    const size_t written = test_memory::residentBytes();
    REQUIRE(clockwork::pages::zero(p, kBytes) == clockwork::pages::Zero::Done);
    const size_t given = test_memory::residentBytes();
    INFO("untouched " << (untouched >> 20) << " MB, written " << (written >> 20) << " MB, given back "
                      << (given >> 20) << " MB");
    CHECK(written >= untouched + kBytes / 2);
    CHECK(given + kBytes / 2 <= written);
    clockwork::pages::unmap(p, kBytes);
}
