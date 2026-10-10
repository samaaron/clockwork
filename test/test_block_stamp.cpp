// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_block_stamp.cpp — when a block is heard, on Link's clock.
 *
 * Every case hands the stamp Link's clock as a callback would have read it, so
 * there is no real clock here, no sleeping, and each case has one answer
 * whatever the machine is doing.
 *
 * Contract pinned (clock/BlockStamp.h):
 *   - the first block after an anchor is stamped when Link's clock says;
 *   - after that, the samples say how far apart two blocks are, and Link's
 *     clock only steers the line, by at most kMaxSteerPpm of each block —
 *     a late callback does not move its block, an early one is not pulled
 *     back to the clock;
 *   - so stamps always go forward, by a block less at most the steer;
 *   - fallen more than kStepMicros behind is time that went, and taken whole;
 *   - on a freewheel clock a stamp is its samples alone;
 *   - a reset anchors afresh; a Link clock reading 0 (no Link) stamps 0.
 *
 * Stamps are whole microseconds, so each expected answer is to within one.
 */
#include <catch2/catch_test_macros.hpp>

#include "clock/BlockStamp.h"

#include <cstdint>
#include <cstdlib>

using clockwork::BlockStamp;

namespace {

constexpr double  kSr          = 48000.0;
constexpr double  kBlock       = 128.0;
constexpr double  kBlockMicros = kBlock / kSr * 1e6;   // 2666.67
constexpr int64_t kStart       = 1'000'000'000;        // Link's clock at the anchor
constexpr bool    kDevice      = false;                // not a freewheel clock
constexpr bool    kFreewheel   = true;

// One block's worth of steer, the most a block may be moved toward the clock.
constexpr double kSteerMicros = BlockStamp::kMaxSteerPpm * 1e-6 * kBlockMicros;

bool within1(int64_t got, double want) {
    return std::llabs(got - std::llround(want)) <= 1;
}

}  // namespace

TEST_CASE("block stamp: the first block after an anchor is stamped when Link's clock says",
          "[clock][block-stamp]") {
    BlockStamp stamp;
    CHECK(stamp.stamp(kStart, 0.0, kSr, kDevice) == kStart);

    // Wherever the sample count stands: a device restarting mid-stream.
    BlockStamp later;
    CHECK(later.stamp(kStart, 480'000.0, kSr, kDevice) == kStart);
}

TEST_CASE("block stamp: on time, a block is stamped a block on", "[clock][block-stamp]") {
    BlockStamp stamp;
    stamp.stamp(kStart, 0.0, kSr, kDevice);
    for (int i = 1; i <= 1000; ++i) {
        const double due = kStart + i * kBlockMicros;
        INFO("block " << i);
        CHECK(within1(stamp.stamp(int64_t(due), i * kBlock, kSr, kDevice), due));
    }
}

TEST_CASE("block stamp: a late callback does not move its block", "[clock][block-stamp]") {
    // The clock is read when the callback runs: 30 ms late reads 30 ms more.
    // Following it would move the timeline a peer's audio is placed on by
    // the lateness, heard as a bend; the samples hold it, steered by a
    // block's worth at most.
    BlockStamp stamp;
    stamp.stamp(kStart, 0.0, kSr, kDevice);
    const int64_t late = stamp.stamp(int64_t(kStart + kBlockMicros + 30'000), kBlock, kSr, kDevice);
    CHECK(within1(late, kStart + kBlockMicros + kSteerMicros));
}

TEST_CASE("block stamp: a block rendered ahead of Link's clock is stamped by its samples",
          "[clock][block-stamp]") {
    // A host pumping blocks, a burst after a late callback: the clock has
    // hardly moved, but 4800 samples have. Pulled back to the clock, a later
    // block would be stamped no later than an earlier one — a stall in the
    // timeline, heard as a skip.
    BlockStamp stamp;
    stamp.stamp(kStart, 0.0, kSr, kDevice);
    const int64_t ahead = stamp.stamp(kStart, 4800.0, kSr, kDevice);
    CHECK(within1(ahead, kStart + 100'000 - BlockStamp::kMaxSteerPpm * 1e-6 * 100'000));
}

TEST_CASE("block stamp: stamps go forward by a block, less the steer at most, however the callbacks come",
          "[clock][block-stamp]") {
    // Callbacks on time, late by up to just under a step, and in bursts with
    // the clock standing still: no gap is ever less than a block steered by a
    // block's worth, so no later block is stamped at or before an earlier one.
    // A block is 2666.67 us, a full steer 2.67 us, and stamps are whole
    // microseconds, which truncation can cost one more.
    constexpr int64_t kLeastGap = 2663;
    const double lateness[] = {0, 30'000, 5'000, 45'000, 0, 0, 49'000, 12'000, 0};
    BlockStamp stamp;
    int64_t last = stamp.stamp(kStart, 0.0, kSr, kDevice);
    double  wall = kStart;
    int     i    = 0;
    for (int round = 0; round < 20; ++round) {
        for (const double late : lateness) {
            ++i;
            wall += kBlockMicros;
            const int64_t now = stamp.stamp(int64_t(wall + late), i * kBlock, kSr, kDevice);
            INFO("block " << i << ", " << late << " us late");
            CHECK(now - last >= kLeastGap);
            last = now;
        }
        for (int burst = 0; burst < 100; ++burst) {   // rendered at once
            ++i;
            const int64_t now = stamp.stamp(int64_t(wall), i * kBlock, kSr, kDevice);
            INFO("block " << i << ", in a burst");
            CHECK(now - last >= kLeastGap);
            last = now;
        }
        wall += 100 * kBlockMicros;   // the burst's samples, heard on the wall's time
    }
}

TEST_CASE("block stamp: a block fallen more than a step behind jumps forward with Link's clock",
          "[clock][block-stamp]") {
    // Time that really went — a sleep, a stalled device — is taken whole: the
    // block is heard when it is heard, and the next from there.
    BlockStamp stamp;
    stamp.stamp(kStart, 0.0, kSr, kDevice);
    const double behind = kBlockMicros + BlockStamp::kStepMicros + 1'000;
    CHECK(within1(stamp.stamp(int64_t(kStart + behind), kBlock, kSr, kDevice), kStart + behind));
    const double next = kStart + behind + kBlockMicros;
    CHECK(within1(stamp.stamp(int64_t(next), 2 * kBlock, kSr, kDevice), next));
}

TEST_CASE("block stamp: on a freewheel clock a stamp is its samples alone", "[clock][block-stamp]") {
    // Deterministic rendering: however late or early the blocks come, the
    // timeline is the samples' from the anchor.
    BlockStamp stamp;
    stamp.stamp(kStart, 0.0, kSr, kFreewheel);
    CHECK(within1(stamp.stamp(kStart + 120'000, kBlock, kSr, kFreewheel), kStart + kBlockMicros));
    CHECK(within1(stamp.stamp(kStart + 120'000, kBlock + kSr, kSr, kFreewheel),
                  kStart + kBlockMicros + 1'000'000));
    CHECK(within1(stamp.stamp(kStart, 2 * kSr, kSr, kFreewheel), kStart + 2'000'000));
}

TEST_CASE("block stamp: a reset anchors afresh on the next block", "[clock][block-stamp]") {
    BlockStamp stamp;
    stamp.stamp(kStart, 0.0, kSr, kFreewheel);
    stamp.stamp(kStart, kSr, kSr, kFreewheel);
    stamp.reset();
    CHECK(stamp.stamp(kStart + 5'000'000, 0.0, kSr, kFreewheel) == kStart + 5'000'000);
}

TEST_CASE("block stamp: no Link clock stamps 0, and anchors on the first reading",
          "[clock][block-stamp]") {
    BlockStamp stamp;
    CHECK(stamp.stamp(0, 0.0, kSr, kDevice) == 0);
    CHECK(stamp.stamp(kStart, kBlock, kSr, kDevice) == kStart);
}

TEST_CASE("block stamp: with no sample rate, a block is stamped when Link's clock says",
          "[clock][block-stamp]") {
    BlockStamp stamp;
    CHECK(stamp.stamp(kStart, kBlock, 0.0, kDevice) == kStart);
}
