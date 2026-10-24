// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_metrics_history.cpp — the metrics history (metrics_history.h): for
 * every counter and gauge, three rings of sixty buckets — a second, ten
 * seconds, a minute each — holding the lowest, highest and last value seen
 * in the bucket. The audio thread records once a block into each level's
 * current bucket; a reader aggregates a window of buckets. No engine here:
 * the writer and the reader over a buffer, on an exact clock.
 */
#include <catch2/catch_test_macros.hpp>

#include "metrics_history.h"
#include "metrics_schema.h"

#include <array>
#include <cstring>
#include <vector>

using namespace clockwork::metrics_history;
namespace schema = clockwork::metrics_schema;

namespace {

constexpr uint32_t kProcessCount = 0;    // a counter
constexpr uint32_t kSchedDepth   = 3;    // a gauge
constexpr uint32_t kSampleRate   = 42;   // a constant
constexpr uint32_t kDriftUs      = 50;   // a signed gauge
constexpr double   kT0 = 3900000000.0;   // an NTP instant, on a minute

struct Rig {
    std::vector<uint8_t> region = std::vector<uint8_t>(kRegionBytes, 0);
    std::array<uint32_t, schema::kMetricWords> words{};
    Writer writer;
    Rig() { writer.bind(region.data()); }
    void block(double ntp) { writer.record(words.data(), ntp); }
    Window at(uint32_t level, uint32_t buckets, uint32_t offset) const {
        return window(region.data(), level, buckets, offset);
    }
};

}  // namespace

TEST_CASE("metrics history: the schema gives every counter and gauge a slot, and none to a constant", "[metrics][history]") {
    REQUIRE(schema::kMetricWords == 80);
    REQUIRE(schema::kMetricHistorySlot[kSampleRate] == -1);
    REQUIRE(schema::kMetricHistorySlot[kProcessCount] >= 0);
    REQUIRE(schema::kMetricHistorySlot[kDriftUs] >= 0);
    REQUIRE(schema::kMetricSigned[kDriftUs]);
    REQUIRE_FALSE(schema::kMetricSigned[kProcessCount]);
    REQUIRE(kMetrics == schema::kMetricHistoryCount);
    REQUIRE(kMetrics >= 50);
    REQUIRE(kRegionBytes == kHeaderBytes + kLevels * kBuckets * kMetrics * kWords * 4);
}

TEST_CASE("metrics history: a block records the lowest, highest and last of a gauge in every level's current bucket",
          "[metrics][history]") {
    Rig rig;
    for (auto [dt, v] : { std::pair{0.0, 5u}, std::pair{0.1, 3u}, std::pair{0.2, 9u} }) {
        rig.words[kSchedDepth] = v;
        rig.block(kT0 + dt);
    }
    for (uint32_t level = 0; level < kLevels; ++level) {
        const Window w = rig.at(level, 1, kSchedDepth);
        INFO("level " << level);
        REQUIRE(w.covered == 1);
        CHECK(w.low == 3);
        CHECK(w.high == 9);
        CHECK(w.last == 9);
    }
}

TEST_CASE("metrics history: a constant has no history", "[metrics][history]") {
    Rig rig;
    rig.words[kSampleRate] = 48000;
    rig.block(kT0);
    CHECK(rig.at(0, 1, kSampleRate).covered == 0);
}

TEST_CASE("metrics history: a signed gauge is ordered as signed", "[metrics][history]") {
    Rig rig;
    rig.words[kDriftUs] = static_cast<uint32_t>(int32_t{-5}); rig.block(kT0);
    rig.words[kDriftUs] = static_cast<uint32_t>(int32_t{3});  rig.block(kT0 + 0.01);
    const Window w = rig.at(0, 1, kDriftUs);
    CHECK(w.low == -5);
    CHECK(w.high == 3);
    CHECK(w.last == 3);
}

TEST_CASE("metrics history: the second steps the bucket on, and seconds with no block hold the last value",
          "[metrics][history]") {
    Rig rig;
    rig.words[kSchedDepth] = 7; rig.block(kT0);
    rig.words[kSchedDepth] = 2; rig.block(kT0 + 5.5);   // no block for five seconds
    // Level 0: buckets 0..5 — 7 held through 1..4, then 2.
    const Window one = rig.at(0, 1, kSchedDepth);
    CHECK(one.covered == 1); CHECK(one.low == 2); CHECK(one.high == 2);
    const Window six = rig.at(0, 6, kSchedDepth);
    CHECK(six.covered == 6); CHECK(six.low == 2); CHECK(six.high == 7); CHECK(six.last == 2);
    // Asked for more than has been recorded: what there is.
    CHECK(rig.at(0, 60, kSchedDepth).covered == 6);
    // Level 1 (ten seconds a bucket): both blocks in the one bucket.
    const Window ten = rig.at(1, 1, kSchedDepth);
    CHECK(ten.covered == 1); CHECK(ten.low == 2); CHECK(ten.high == 7);
}

TEST_CASE("metrics history: a window gives a counter's change across it", "[metrics][history]") {
    Rig rig;
    rig.words[kProcessCount] = 100; rig.block(kT0);
    rig.words[kProcessCount] = 110; rig.block(kT0 + 0.5);
    rig.words[kProcessCount] = 130; rig.block(kT0 + 1.5);
    // The current second alone: from the last of the second before.
    CHECK(rig.at(0, 1, kProcessCount).delta == 20);
    // Both seconds: from the first value recorded.
    CHECK(rig.at(0, 2, kProcessCount).delta == 30);
    CHECK(rig.at(0, 2, kProcessCount).last == 130);
}

TEST_CASE("metrics history: the rings wrap, and a window never reaches past what they hold", "[metrics][history]") {
    Rig rig;
    for (int s = 0; s < 150; ++s) {        // two and a half minutes, a block a second
        rig.words[kSchedDepth] = static_cast<uint32_t>(s);
        rig.block(kT0 + s);
    }
    const Window minute = rig.at(0, 60, kSchedDepth);
    CHECK(minute.covered == 60);
    CHECK(minute.low == 90);               // the seconds the ring still holds: 90..149
    CHECK(minute.high == 149);
    CHECK(minute.last == 149);
    const Window tens = rig.at(1, 60, kSchedDepth);
    CHECK(tens.covered == 15);             // 150 s is fifteen ten-second buckets
    CHECK(tens.low == 0);
    CHECK(tens.high == 149);
    const Window minutes = rig.at(2, 60, kSchedDepth);
    CHECK(minutes.covered == 3);           // the third minute part-way
    CHECK(minutes.low == 0);
}

TEST_CASE("metrics history: a gap longer than a ring fills the whole ring with the held value",
          "[metrics][history]") {
    Rig rig;
    rig.words[kSchedDepth] = 4; rig.block(kT0);
    rig.words[kSchedDepth] = 1; rig.block(kT0 + 3600.0);   // an hour with no block
    const Window minute = rig.at(0, 60, kSchedDepth);
    CHECK(minute.covered == 60);
    CHECK(minute.low == 1);
    CHECK(minute.high == 4);               // the held value through 59 buckets, then 1
    const Window hour = rig.at(2, 60, kSchedDepth);
    CHECK(hour.covered == 60);
    CHECK(hour.high == 4);
    CHECK(hour.last == 1);
}
