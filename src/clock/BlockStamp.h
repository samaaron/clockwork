// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * BlockStamp.h — when a block is heard, on Link's clock.
 *
 * Link Audio places a peer's audio on Link's per-boot micros, so each block
 * the engine renders is stamped in that domain: when its first sample is
 * heard. The samples say how far apart two blocks are; Link's clock, read as
 * the callback runs, only says how far that has wandered.
 *
 * The rule alone, handed the clock's reading rather than reading it, so that
 * it is tested on made-up times (test_block_stamp.cpp); LinkAudioHost reads
 * Link's clock and keeps one of these.
 */
#pragma once

#include "clock/clock_math.h"   // kDriftIirGain

#include <algorithm>
#include <cstdint>

namespace clockwork {

class BlockStamp {
public:
    // The most each block's stamp is steered toward Link's clock, as a share
    // of its own length: more than any two crystals disagree by.
    static constexpr double kMaxSteerPpm = 1000.0;
    // Fallen this far behind Link's clock is time that went — a sleep, a
    // stalled device — and is taken whole.
    static constexpr double kStepMicros = 50'000.0;

    // The stamp of the block at `samplePosition`, Link's clock reading
    // `linkNowMicros` as its callback runs. The first block after an anchor is
    // stamped with the reading; each after it by its samples, steered toward
    // the clock and never stepped onto it. A callback that runs late reads the
    // clock late, and following that would step every stamp by a share of the
    // lateness: a step in the timeline peers' audio is placed on, heard as a
    // bend. Ahead is never stepped back: a host rendering early, or a burst
    // after a late callback, has lost no time, and stepping back would stamp a
    // later block no later than an earlier one. (The NTP in TimeSource follows
    // the same IIR unbounded; a scheduler can take a step that audio can't.)
    //
    // On a freewheel clock (deterministic rendering) a stamp is its samples
    // alone, from the anchor. A reading of 0 is no Link clock, and stamps 0.
    int64_t stamp(int64_t linkNowMicros, double samplePosition, double sampleRate, bool freewheel) {
        if (linkNowMicros == 0) return 0;
        if (sampleRate <= 0.0) return linkNowMicros;
        const double offsetMicros = samplePosition / sampleRate * 1e6;
        const double linkNow      = static_cast<double>(linkNowMicros);

        if (!mAnchored) {
            mBaseMicros = linkNow - offsetMicros;
            mAnchored   = true;
        } else if (!freewheel) {
            const double drift = linkNow - (mBaseMicros + offsetMicros);
            if (drift > kStepMicros) {
                mBaseMicros += drift;
            } else {
                const double steer =
                    kMaxSteerPpm * 1e-6 * std::max(0.0, offsetMicros - mLastOffsetMicros);
                mBaseMicros += std::clamp(drift * kDriftIirGain, -steer, steer);
            }
        }
        mLastOffsetMicros = offsetMicros;
        return static_cast<int64_t>(mBaseMicros + offsetMicros);
    }

    // Anchor afresh on the next stamp: wherever a driver resets the clock's
    // audio-thread time (device start, the first manual block).
    void reset() { mAnchored = false; }

private:
    double mBaseMicros{0.0};         // sample 0's place on Link's clock
    double mLastOffsetMicros{0.0};   // the last block's place on the sample clock
    bool   mAnchored{false};
};

}  // namespace clockwork
