// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#pragma once

// How long a stretch of renders took, for the processor's line ([load]) on a
// minute that had a render over half its budget: start to finish and on the
// CPU, as the middle one, the 99th in 100 and the longest. Each render is
// kept as a count in a step of a hundredth of its block's budget (the last
// step: twice the budget or more), so nothing is allocated and a summary
// costs a walk of the steps. The audio thread's alone.

#include <array>
#include <cstdint>

namespace clockwork {

class RenderSpread {
public:
    static constexpr int kSteps = 201;   // 0-199 % of the budget, and 200 % or more

    struct Summary {
        double seconds  = 0.0;           // the stretch it covers
        double budgetUs = 0.0;           // the latest render's budget: what the steps are hundredths of
        uint32_t renders = 0, overBudget = 0;
        double wallMedianUs = 0.0, wall99Us = 0.0, wallLongestUs = 0.0;
        uint32_t cpuRenders = 0;         // renders whose CPU time was known: none on Windows
        double cpuMedianUs = 0.0, cpu99Us = 0.0, cpuLongestUs = 0.0;
        // Worth a line of the log: a render went over its budget, or took
        // more than half of it. A stretch of ordinary renders is not.
        bool worthSaying() const { return overBudget > 0 || wallLongestUs > budgetUs * 0.5; }
    };

    // One render, at `nowSec` on the monotonic clock: how long it took start
    // to finish and on the CPU (negative: not known), against its budget.
    void note(double wallUs, double cpuUs, double budgetUs, double nowSec) {
        if (!(budgetUs > 0.0)) return;
        if (mStartSec <= 0.0) mStartSec = nowSec;
        mBudgetUs = budgetUs;
        ++mWall[stepOf(wallUs, budgetUs)];
        ++mRenders;
        if (wallUs > budgetUs) ++mOver;
        if (wallUs > mWallLongestUs) mWallLongestUs = wallUs;
        if (cpuUs >= 0.0) {
            ++mCpu[stepOf(cpuUs, budgetUs)];
            ++mCpuRenders;
            if (cpuUs > mCpuLongestUs) mCpuLongestUs = cpuUs;
        }
    }

    // Whether the stretch has run `periodSec` by `nowSec`.
    bool due(double nowSec, double periodSec) const {
        return mRenders > 0 && nowSec - mStartSec >= periodSec;
    }

    // The stretch so far, which starts the next.
    Summary take(double nowSec) {
        Summary s;
        s.seconds       = nowSec - mStartSec;
        s.budgetUs      = mBudgetUs;
        s.renders       = mRenders;
        s.overBudget    = mOver;
        s.wallMedianUs  = at(mWall, mRenders, 0.5, mWallLongestUs);
        s.wall99Us      = at(mWall, mRenders, 0.99, mWallLongestUs);
        s.wallLongestUs = mWallLongestUs;
        s.cpuRenders    = mCpuRenders;
        if (mCpuRenders > 0) {
            s.cpuMedianUs  = at(mCpu, mCpuRenders, 0.5, mCpuLongestUs);
            s.cpu99Us      = at(mCpu, mCpuRenders, 0.99, mCpuLongestUs);
            s.cpuLongestUs = mCpuLongestUs;
        }
        *this = RenderSpread{};
        mStartSec = nowSec;
        return s;
    }

private:
    static int stepOf(double us, double budgetUs) {
        const double step = us / budgetUs * 100.0;
        return step >= kSteps - 1 ? kSteps - 1 : (step > 0.0 ? static_cast<int>(step) : 0);
    }

    // The render at fraction `q` of `count`, as the top of its step, in µs;
    // in the last step, which has no top, the longest.
    double at(const std::array<uint32_t, kSteps>& steps, uint32_t count, double q, double longestUs) const {
        const auto rank = static_cast<uint64_t>(q * count + 0.999999);   // the ceil, 1-based
        uint64_t seen = 0;
        for (int i = 0; i < kSteps; ++i) {
            seen += steps[i];
            if (seen >= rank)
                return i == kSteps - 1 ? longestUs : (i + 1) * mBudgetUs / 100.0;
        }
        return longestUs;
    }

    std::array<uint32_t, kSteps> mWall{}, mCpu{};
    uint32_t mRenders = 0, mOver = 0, mCpuRenders = 0;
    double mWallLongestUs = 0.0, mCpuLongestUs = 0.0;
    double mBudgetUs = 0.0;
    double mStartSec = 0.0;
};

}  // namespace clockwork
