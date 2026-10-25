// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * metrics_history.h — what every counter and gauge did over the last hour.
 *
 * The metrics block (PerformanceMetrics) is one set of current values; a
 * reader polling it sees nothing between two reads, and loses what it saw
 * with its page. The history keeps, for every metric that is not a constant
 * (metrics_schema.h: kMetricHistorySlot), three rings of kBuckets buckets —
 * a second, ten seconds and a minute each (kSpanSeconds) — and in each
 * bucket three words: the lowest value seen in it, the highest, and the
 * last. The audio thread records once a block (Writer::record, reading the
 * block's words as the atomics they are: other threads count into them): for each
 * level, the block's values into the current bucket, a compare and a store
 * each; when the engine's clock crosses a bucket's span the level steps on,
 * and buckets no block fell in hold the last value (a gap is a hold, not a
 * hole), at most a ring's worth whatever the gap. Fixed memory, no
 * allocation, a few hundred operations a block; deterministic, on the
 * engine's own clock.
 *
 * Readers aggregate (window): the lowest and highest over the last n buckets
 * of a level, the last value, and the change across the window — for a
 * counter, what it counted in that time. The last second, ten seconds and
 * minute from level 0; ten minutes from level 1; the hour from level 2. A
 * window reaching past what has been recorded says how many buckets it
 * covered. Reads are relaxed, and a bucket mid-update reads a block early
 * or late, which a display cannot tell.
 *
 * Layout (metrics-history region, docs/ARENA): a header of kHeaderBytes —
 * the origin the buckets count from (f64 NTP, on a minute, set at the first
 * record), each level's current bucket number and its first — then the
 * levels, each kBuckets × kMetrics × kWords words, bucket-major so a block
 * touches one run per level. The region is reset at a cold boot only: a
 * rebuild or a new worklet over the same memory carries the history on.
 */
#pragma once

#include "metrics_schema.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace clockwork {
namespace metrics_history {

inline constexpr uint32_t kLevels  = 3;
inline constexpr uint32_t kBuckets = 60;
inline constexpr uint32_t kSpanSeconds[kLevels] = { 1, 10, 60 };
inline constexpr uint32_t kWords   = 3;   // low, high, last
inline constexpr uint32_t kMetrics = metrics_schema::kMetricHistoryCount;
inline constexpr uint32_t kHeaderBytes = 64;
inline constexpr uint32_t kBucketWords = kMetrics * kWords;
inline constexpr uint32_t kLevelBytes  = kBuckets * kBucketWords * 4;
inline constexpr uint32_t kRegionBytes = kHeaderBytes + kLevels * kLevelBytes;
static_assert(kRegionBytes % 16 == 0, "the history region keeps the arena's alignment");

// Header words.
inline constexpr uint32_t kHdrOriginLo = 0;    // the origin NTP, f64 bits, low word then high
inline constexpr uint32_t kHdrOriginHi = 1;
inline constexpr uint32_t kHdrCurrent  = 2;    // + level: the current bucket number
inline constexpr uint32_t kHdrFirst    = 5;    // + level: the first bucket number recorded
inline constexpr uint32_t kNoBucket    = 0xFFFFFFFFu;

inline std::atomic<uint32_t>* words(uint8_t* region) {
    return reinterpret_cast<std::atomic<uint32_t>*>(region);
}
inline const std::atomic<uint32_t>* words(const uint8_t* region) {
    return reinterpret_cast<const std::atomic<uint32_t>*>(region);
}
inline uint32_t slotWord(uint32_t level, uint32_t bucketIndex, uint32_t slot) {
    return kHeaderBytes / 4 + level * (kBuckets * kBucketWords) + bucketIndex * kBucketWords + slot * kWords;
}
inline int64_t asValue(uint32_t raw, bool isSigned) {
    return isSigned ? static_cast<int64_t>(static_cast<int32_t>(raw)) : static_cast<int64_t>(raw);
}
inline bool lower(uint32_t a, uint32_t b, bool isSigned) {
    return asValue(a, isSigned) < asValue(b, isSigned);
}

// A window over the last `covered` buckets of a level.
struct Window {
    int64_t  low = 0;
    int64_t  high = 0;
    int64_t  last = 0;
    int64_t  delta = 0;     // last, less the last before the window (or the window's first low)
    uint32_t covered = 0;   // buckets the window found recorded; 0: nothing, or no history for this metric
};

// The audio thread's writer. Bound once to the region; record() once a block.
class Writer {
public:
    void bind(uint8_t* region) { mRegion = region; }

    void record(const std::atomic<uint32_t>* metricWords, double ntp) {
        if (!mRegion) return;
        std::atomic<uint32_t>* w = words(mRegion);
        const uint64_t originBits = (static_cast<uint64_t>(w[kHdrOriginHi].load(std::memory_order_relaxed)) << 32)
                                  | w[kHdrOriginLo].load(std::memory_order_relaxed);
        double origin;
        if (originBits == 0) {
            // The first record since the cold boot: the buckets of every
            // level count from the minute this block fell in, so the levels'
            // boundaries line up.
            origin = std::floor(ntp / 60.0) * 60.0;
            uint64_t bits;
            std::memcpy(&bits, &origin, sizeof bits);
            w[kHdrOriginLo].store(static_cast<uint32_t>(bits), std::memory_order_relaxed);
            w[kHdrOriginHi].store(static_cast<uint32_t>(bits >> 32), std::memory_order_relaxed);
            for (uint32_t level = 0; level < kLevels; ++level) {
                const uint32_t b = bucketOf(ntp, origin, level);
                w[kHdrFirst + level].store(b, std::memory_order_relaxed);
                w[kHdrCurrent + level].store(b, std::memory_order_relaxed);
                fill(w, level, b % kBuckets, metricWords);
            }
            return;
        }
        std::memcpy(&origin, &originBits, sizeof origin);
        for (uint32_t level = 0; level < kLevels; ++level) {
            const uint32_t target = bucketOf(ntp, origin, level);
            uint32_t current = w[kHdrCurrent + level].load(std::memory_order_relaxed);
            if (target > current) {
                // Step on: the buckets between hold the last of the one
                // before them (no block fell in them), at most a ring's worth
                // whatever the gap; the target bucket begins with this block.
                uint32_t from = current % kBuckets;   // the last bucket recorded
                if (target - current > kBuckets) current = target - kBuckets;
                while (current + 1 < target) {
                    ++current;
                    hold(w, level, current % kBuckets, from);
                    from = current % kBuckets;
                }
                current = target;
                fill(w, level, current % kBuckets, metricWords);
                w[kHdrCurrent + level].store(current, std::memory_order_relaxed);
            } else {
                update(w, level, current % kBuckets, metricWords);
            }
        }
    }

private:
    static uint32_t bucketOf(double ntp, double origin, uint32_t level) {
        const double since = std::max(0.0, ntp - origin);
        return static_cast<uint32_t>(since / kSpanSeconds[level]);
    }
    // A bucket's first block: low, high and last are the block's values.
    static void fill(std::atomic<uint32_t>* w, uint32_t level, uint32_t index, const std::atomic<uint32_t>* metricWords) {
        for (uint32_t offset = 0; offset < metrics_schema::kMetricWords; ++offset) {
            const int8_t slot = metrics_schema::kMetricHistorySlot[offset];
            if (slot < 0) continue;
            const uint32_t v = metricWords[offset].load(std::memory_order_relaxed);
            const uint32_t i = slotWord(level, index, static_cast<uint32_t>(slot));
            w[i].store(v, std::memory_order_relaxed);
            w[i + 1].store(v, std::memory_order_relaxed);
            w[i + 2].store(v, std::memory_order_relaxed);
        }
    }
    // A bucket no block fell in: it holds the last of the bucket before.
    static void hold(std::atomic<uint32_t>* w, uint32_t level, uint32_t index, uint32_t fromIndex) {
        for (uint32_t slot = 0; slot < kMetrics; ++slot) {
            const uint32_t v = w[slotWord(level, fromIndex, slot) + 2].load(std::memory_order_relaxed);
            const uint32_t i = slotWord(level, index, slot);
            w[i].store(v, std::memory_order_relaxed);
            w[i + 1].store(v, std::memory_order_relaxed);
            w[i + 2].store(v, std::memory_order_relaxed);
        }
    }
    // A block in a bucket already begun: the low and high move if the value
    // is beyond them; the last is the value.
    static void update(std::atomic<uint32_t>* w, uint32_t level, uint32_t index, const std::atomic<uint32_t>* metricWords) {
        for (uint32_t offset = 0; offset < metrics_schema::kMetricWords; ++offset) {
            const int8_t slot = metrics_schema::kMetricHistorySlot[offset];
            if (slot < 0) continue;
            const bool isSigned = metrics_schema::kMetricSigned[offset];
            const uint32_t v = metricWords[offset].load(std::memory_order_relaxed);
            const uint32_t i = slotWord(level, index, static_cast<uint32_t>(slot));
            if (lower(v, w[i].load(std::memory_order_relaxed), isSigned))     w[i].store(v, std::memory_order_relaxed);
            if (lower(w[i + 1].load(std::memory_order_relaxed), v, isSigned)) w[i + 1].store(v, std::memory_order_relaxed);
            w[i + 2].store(v, std::memory_order_relaxed);
        }
    }

    uint8_t* mRegion = nullptr;
};

// One bucket of a level, `back` buckets before the current (0: the current,
// part-way through), for the metric at `metricOffset`. `recorded` is false
// past what the level holds, or for a metric with no history.
struct Bucket {
    int64_t low = 0;
    int64_t high = 0;
    int64_t last = 0;
    bool    recorded = false;
};
inline Bucket bucket(const uint8_t* region, uint32_t level, uint32_t back, uint32_t metricOffset) {
    Bucket out;
    if (!region || level >= kLevels || back >= kBuckets || metricOffset >= metrics_schema::kMetricWords) return out;
    const int8_t slot = metrics_schema::kMetricHistorySlot[metricOffset];
    if (slot < 0) return out;
    const bool isSigned = metrics_schema::kMetricSigned[metricOffset];
    const std::atomic<uint32_t>* w = words(region);
    const uint32_t first   = w[kHdrFirst + level].load(std::memory_order_relaxed);
    const uint32_t current = w[kHdrCurrent + level].load(std::memory_order_relaxed);
    if (first == kNoBucket || (w[kHdrOriginLo].load(std::memory_order_relaxed) == 0 && w[kHdrOriginHi].load(std::memory_order_relaxed) == 0))
        return out;
    if (back > current - first) return out;   // before the first recorded
    const uint32_t i = slotWord(level, (current - back) % kBuckets, static_cast<uint32_t>(slot));
    out.low  = asValue(w[i].load(std::memory_order_relaxed), isSigned);
    out.high = asValue(w[i + 1].load(std::memory_order_relaxed), isSigned);
    out.last = asValue(w[i + 2].load(std::memory_order_relaxed), isSigned);
    out.recorded = true;
    return out;
}

// A reader, any thread: the last `buckets` buckets (1..kBuckets) of `level`
// for the metric at `metricOffset` in the block.
inline Window window(const uint8_t* region, uint32_t level, uint32_t buckets, uint32_t metricOffset) {
    Window out;
    if (!region || level >= kLevels || buckets == 0 || metricOffset >= metrics_schema::kMetricWords) return out;
    const int8_t slot = metrics_schema::kMetricHistorySlot[metricOffset];
    if (slot < 0) return out;
    const bool isSigned = metrics_schema::kMetricSigned[metricOffset];
    const std::atomic<uint32_t>* w = words(region);
    const uint32_t first   = w[kHdrFirst + level].load(std::memory_order_relaxed);
    const uint32_t current = w[kHdrCurrent + level].load(std::memory_order_relaxed);
    if (first == kNoBucket || (w[kHdrOriginLo].load(std::memory_order_relaxed) == 0 && w[kHdrOriginHi].load(std::memory_order_relaxed) == 0))
        return out;
    buckets = std::min(buckets, kBuckets);
    // The window: current - covered + 1 .. current, within what is recorded and what the ring holds.
    const uint32_t recorded = current - first + 1;
    const uint32_t covered  = std::min(buckets, std::min(recorded, kBuckets));
    const uint32_t start    = current - covered + 1;
    for (uint32_t b = start; b <= current; ++b) {
        const uint32_t i = slotWord(level, b % kBuckets, static_cast<uint32_t>(slot));
        const int64_t lo = asValue(w[i].load(std::memory_order_relaxed), isSigned);
        const int64_t hi = asValue(w[i + 1].load(std::memory_order_relaxed), isSigned);
        if (b == start) { out.low = lo; out.high = hi; }
        else { out.low = std::min(out.low, lo); out.high = std::max(out.high, hi); }
    }
    out.last = asValue(w[slotWord(level, current % kBuckets, static_cast<uint32_t>(slot)) + 2].load(std::memory_order_relaxed), isSigned);
    // The change: from the last of the bucket before the window where the
    // ring still holds one, else from the window's first low.
    const bool before = start > first && (current - (start - 1)) < kBuckets;
    if (before) {
        const int64_t prev = asValue(w[slotWord(level, (start - 1) % kBuckets, static_cast<uint32_t>(slot)) + 2].load(std::memory_order_relaxed), isSigned);
        out.delta = out.last - prev;
    } else {
        const int64_t firstLow = asValue(w[slotWord(level, start % kBuckets, static_cast<uint32_t>(slot))].load(std::memory_order_relaxed), isSigned);
        out.delta = out.last - firstLow;
    }
    out.covered = covered;
    return out;
}

}  // namespace metrics_history
}  // namespace clockwork
