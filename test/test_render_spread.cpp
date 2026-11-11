// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_render_spread.cpp — how long a stretch of renders took
 * (clockwork::RenderSpread, src/native/RenderSpread.h), the processor's
 * [load] line after a minute with a render over half its budget: the middle
 * render, the 99th in 100 and the longest, start to finish and on the CPU,
 * against the block's budget.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "native/RenderSpread.h"

using clockwork::RenderSpread;
using Catch::Matchers::WithinAbs;

namespace {
constexpr double kBudgetUs = 1000.0;   // a block's budget: each step is 10 µs of it
}

TEST_CASE("render spread: the middle, the 99th in 100 and the longest, each to a hundredth of the budget",
          "[load]") {
    RenderSpread spread;
    // 98 renders at a tenth of the budget, one at half and one at one and a half.
    for (int i = 0; i < 98; ++i) spread.note(100.0, 90.0, kBudgetUs, 1.0);
    spread.note(500.0, 450.0, kBudgetUs, 1.0);
    spread.note(1500.0, 300.0, kBudgetUs, 1.0);
    const auto s = spread.take(61.0);
    CHECK(s.renders == 100u);
    CHECK(s.overBudget == 1u);
    CHECK_THAT(s.seconds, WithinAbs(60.0, 1e-9));
    CHECK_THAT(s.budgetUs, WithinAbs(kBudgetUs, 1e-9));
    // As the top of a hundredth: 100 µs is in the step that ends at 110.
    CHECK_THAT(s.wallMedianUs, WithinAbs(110.0, 1e-9));
    CHECK_THAT(s.wall99Us, WithinAbs(510.0, 1e-9));
    CHECK_THAT(s.wallLongestUs, WithinAbs(1500.0, 1e-9));
    // The CPU's own: the long render was mostly waiting.
    CHECK(s.cpuRenders == 100u);
    CHECK_THAT(s.cpuMedianUs, WithinAbs(100.0, 1e-9));
    CHECK_THAT(s.cpu99Us, WithinAbs(310.0, 1e-9));
    CHECK_THAT(s.cpuLongestUs, WithinAbs(450.0, 1e-9));
}

TEST_CASE("render spread: twice the budget or more is said as the longest", "[load]") {
    RenderSpread spread;
    spread.note(3000.0, 2500.0, kBudgetUs, 1.0);
    spread.note(5000.0, 2600.0, kBudgetUs, 1.0);
    const auto s = spread.take(2.0);
    CHECK_THAT(s.wallMedianUs, WithinAbs(5000.0, 1e-9));
    CHECK_THAT(s.wall99Us, WithinAbs(5000.0, 1e-9));
    CHECK(s.overBudget == 2u);
    CHECK_THAT(s.cpuMedianUs, WithinAbs(2600.0, 1e-9));
}

TEST_CASE("render spread: a stretch is due once it has run its period, and the next starts empty", "[load]") {
    RenderSpread spread;
    CHECK_FALSE(spread.due(100.0, 60.0));   // nothing rendered: nothing to say
    spread.note(100.0, 90.0, kBudgetUs, 10.0);
    CHECK_FALSE(spread.due(69.9, 60.0));
    CHECK(spread.due(70.0, 60.0));
    CHECK(spread.take(70.0).renders == 1u);
    // The next stretch starts where the last was taken.
    CHECK_FALSE(spread.due(129.9, 60.0));
    spread.note(200.0, 150.0, kBudgetUs, 100.0);
    CHECK(spread.due(130.0, 60.0));
    const auto s = spread.take(130.0);
    CHECK(s.renders == 1u);
    CHECK_THAT(s.wallLongestUs, WithinAbs(200.0, 1e-9));
}

TEST_CASE("render spread: renders whose CPU time is not known leave it out", "[load]") {
    RenderSpread spread;
    spread.note(100.0, -1.0, kBudgetUs, 1.0);   // as on Windows: too coarse to say
    spread.note(200.0, -1.0, kBudgetUs, 1.0);
    const auto s = spread.take(2.0);
    CHECK(s.renders == 2u);
    CHECK(s.cpuRenders == 0u);
    CHECK(s.cpuLongestUs == 0.0);
}

TEST_CASE("render spread: a stretch is worth saying when a render took over half its budget", "[load]") {
    RenderSpread spread;
    for (int i = 0; i < 100; ++i) spread.note(100.0, 90.0, kBudgetUs, 1.0);
    spread.note(500.0, 90.0, kBudgetUs, 1.0);   // half, and no more
    CHECK_FALSE(spread.take(2.0).worthSaying());
    spread.note(501.0, 90.0, kBudgetUs, 3.0);
    CHECK(spread.take(4.0).worthSaying());
    spread.note(1001.0, 90.0, kBudgetUs, 5.0);   // over budget
    const auto over = spread.take(6.0);
    CHECK(over.overBudget == 1u);
    CHECK(over.worthSaying());
}

TEST_CASE("render spread: a render with no budget is not counted", "[load]") {
    RenderSpread spread;
    spread.note(100.0, 90.0, 0.0, 1.0);
    CHECK_FALSE(spread.due(100.0, 60.0));
}
