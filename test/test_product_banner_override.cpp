// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * Compiled with CLOCKWORK_PRODUCT_HEADER naming product_header_fixture.h,
 * the way a product's build names its own: the banner must be the one the
 * header defines, and what the header leaves alone must keep its default.
 */
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "clockwork_product.h"

TEST_CASE("a product header overlays the banner and only the banner", "[product]") {
    CHECK(std::string(CLOCKWORK_PRODUCT_BANNER) == "fixture banner");
    CHECK(std::string(CLOCKWORK_PRODUCT_NAME) == "clockwork");
}
