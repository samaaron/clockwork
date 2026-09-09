// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * The banner the engine prints as it boots is the product's, supplied the
 * same way as its name (src/clockwork_product.h). This unit is compiled
 * without a product header, so what it sees is clockwork's own default;
 * test_product_banner_override.cpp is the same check compiled with one.
 */
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "clockwork_product.h"

namespace {
std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::string::size_type at = 0;
    for (;;) {
        const auto nl = s.find('\n', at);
        out.push_back(s.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) return out;
        at = nl + 1;
    }
}
// Display width: the art is UTF-8 block characters, one column each.
std::size_t columns(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}
}

TEST_CASE("without a product header the banner is clockwork's own", "[product]") {
    const std::string banner = CLOCKWORK_PRODUCT_BANNER;
    CHECK(banner ==
          "░█▀▀░█░░░█▀█░█▀▀░█░█░█░█░█▀█░█▀▄░█░█\n"
          "░█░░░█░░░█░█░█░░░█▀▄░█▄█░█░█░█▀▄░█▀▄\n"
          "░▀▀▀░▀▀▀░▀▀▀░▀▀▀░▀░▀░▀░▀░▀▀▀░▀░▀░▀░▀");
    CHECK(std::string(CLOCKWORK_PRODUCT_NAME) == "clockwork");
}

TEST_CASE("the banner is a rectangle of three lines", "[product]") {
    // Printed as-is by fprintf and clockwork_log: a ragged or four-line
    // banner is a banner nobody proofread.
    const auto rows = lines(CLOCKWORK_PRODUCT_BANNER);
    REQUIRE(rows.size() == 3);
    for (const auto& r : rows) CHECK(columns(r) == columns(rows[0]));
}
