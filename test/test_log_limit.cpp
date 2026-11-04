// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_log_limit.cpp — how often one kind of log line is said (log_limit.h).
 */
#include "log_limit.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using clockwork::LogLimit;
using clockwork::LogSpacing;

namespace {
// Which of `times` asks were told yes, counting from 1.
template <typename Ask>
std::vector<int> said(int times, Ask ask) {
    std::vector<int> yes;
    for (int i = 1; i <= times; ++i)
        if (ask()) yes.push_back(i);
    return yes;
}
} // namespace

TEST_CASE("log limit: the first few times, then not again until reset", "[log-limit]") {
    LogLimit limit = LogLimit::first(3);
    CHECK(said(10, [&] { return limit.say(); }) == std::vector<int>{ 1, 2, 3 });
    limit.reset();
    CHECK(said(10, [&] { return limit.say(); }) == std::vector<int>{ 1, 2, 3 });
}

TEST_CASE("log limit: the first time and every nth after", "[log-limit]") {
    LogLimit limit = LogLimit::every(4);
    CHECK(said(10, [&] { return limit.say(); }) == std::vector<int>{ 1, 5, 9 });
}

TEST_CASE("log limit: threads asking at once are told yes the first few times between them",
          "[log-limit]") {
    LogLimit limit = LogLimit::first(5);
    std::atomic<int> yes{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i)
                if (limit.say()) yes.fetch_add(1, std::memory_order_relaxed);
        });
    for (auto& t : threads) t.join();
    CHECK(yes.load() == 5);
}

TEST_CASE("log spacing: the first line, then none closer than the interval", "[log-limit]") {
    LogSpacing spacing(100);
    CHECK(spacing.say(1000));
    CHECK_FALSE(spacing.say(1000));
    CHECK_FALSE(spacing.say(1099));
    CHECK(spacing.say(1100));
    CHECK_FALSE(spacing.say(1150));
    CHECK(spacing.say(1200));
}

TEST_CASE("log spacing: the first line whatever the clock reads, and again after a reset",
          "[log-limit]") {
    // Clocks that start at 0, and ones far into their range (an NTP time).
    for (uint64_t start : { uint64_t{0}, uint64_t{1} << 63, UINT64_MAX - 1 }) {
        LogSpacing spacing(100);
        CHECK(spacing.say(start));
        CHECK_FALSE(spacing.say(start));
        spacing.reset();
        CHECK(spacing.say(start));
    }
}
