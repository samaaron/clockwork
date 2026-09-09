// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_asset_pool.cpp — the client's pool over the inbox lane
 * (src/clockwork_asset_pool.h).
 *
 * The lane is the client's to carve. This pool hands out offsets into it and
 * takes them back; it holds no memory of its own and never touches the lane,
 * so the same code serves a client writing plain memory in its own process
 * and one writing a mapped segment. Offsets are 16-aligned, because a slot's
 * first byte is where a guest will point a float array; a freed slot is
 * reusable; neighbours coalesce, so churn does not fragment the lane into
 * slivers; and a bad free is refused rather than trusted.
 */
#include <catch2/catch_test_macros.hpp>

#include "clockwork_asset_pool.h"

#include <cstdint>
#include <vector>

TEST_CASE("asset pool: offsets are 16-aligned and inside the lane", "[asset-pool]") {
    ClockworkAssetPool* p = clockwork_asset_pool_open(1 << 20);
    REQUIRE(p != nullptr);
    uint32_t a = 0, b = 0, c = 0;
    REQUIRE(clockwork_asset_pool_alloc(p, 100, &a) == 0);
    REQUIRE(clockwork_asset_pool_alloc(p, 1, &b) == 0);
    REQUIRE(clockwork_asset_pool_alloc(p, 4096, &c) == 0);
    CHECK(a % 16 == 0);
    CHECK(b % 16 == 0);
    CHECK(c % 16 == 0);
    CHECK(b >= a + 100);          // no overlap, however small the request
    CHECK(c >= b + 1);
    CHECK(c + 4096 <= (1u << 20));
    // Nothing for nothing.
    uint32_t z = 0;
    CHECK(clockwork_asset_pool_alloc(p, 0, &z) != 0);
    clockwork_asset_pool_close(p);
}

TEST_CASE("asset pool: a freed slot is reused and neighbours coalesce", "[asset-pool]") {
    ClockworkAssetPool* p = clockwork_asset_pool_open(4096);
    uint32_t a = 0, b = 0, c = 0, d = 0;
    REQUIRE(clockwork_asset_pool_alloc(p, 1024, &a) == 0);
    REQUIRE(clockwork_asset_pool_alloc(p, 1024, &b) == 0);
    REQUIRE(clockwork_asset_pool_alloc(p, 1024, &c) == 0);
    // Full to within the last slot: a fourth 1024 cannot fit (4096 minus
    // three 1024s, less alignment padding, is under 1024).
    CHECK(clockwork_asset_pool_alloc(p, 2048, &d) != 0);

    // Free the middle: something its size fits there again, something larger
    // does not — the two frees on either side are not adjacent to it yet.
    REQUIRE(clockwork_asset_pool_free(p, b) == 0);
    CHECK(clockwork_asset_pool_alloc(p, 2048, &d) != 0);
    REQUIRE(clockwork_asset_pool_alloc(p, 1024, &d) == 0);
    CHECK(d == b);

    // Free a and b together: they coalesce into one 2048 run.
    REQUIRE(clockwork_asset_pool_free(p, a) == 0);
    REQUIRE(clockwork_asset_pool_free(p, d) == 0);
    uint32_t big = 0;
    REQUIRE(clockwork_asset_pool_alloc(p, 2048, &big) == 0);
    CHECK(big == a);
    clockwork_asset_pool_close(p);
}

TEST_CASE("asset pool: exhaustion is a refusal, and freeing everything restores the lane", "[asset-pool]") {
    ClockworkAssetPool* p = clockwork_asset_pool_open(1 << 16);
    std::vector<uint32_t> got;
    uint32_t o = 0;
    while (clockwork_asset_pool_alloc(p, 1000, &o) == 0) got.push_back(o);
    CHECK(got.size() >= 60);
    CHECK(got.size() <= 65);
    for (uint32_t x : got) REQUIRE(clockwork_asset_pool_free(p, x) == 0);
    CHECK(clockwork_asset_pool_free_bytes(p) == (1u << 16));
    // Whole again: one allocation the size of the lane fits.
    REQUIRE(clockwork_asset_pool_alloc(p, 1u << 16, &o) == 0);
    CHECK(o == 0);
    clockwork_asset_pool_close(p);
}

TEST_CASE("asset pool: a free of something never allocated is refused", "[asset-pool]") {
    ClockworkAssetPool* p = clockwork_asset_pool_open(4096);
    uint32_t a = 0;
    REQUIRE(clockwork_asset_pool_alloc(p, 64, &a) == 0);
    CHECK(clockwork_asset_pool_free(p, a + 16) != 0);   // inside a slot, not its start
    CHECK(clockwork_asset_pool_free(p, 8192) != 0);     // outside the lane
    REQUIRE(clockwork_asset_pool_free(p, a) == 0);
    CHECK(clockwork_asset_pool_free(p, a) != 0);        // twice
    clockwork_asset_pool_close(p);
}
