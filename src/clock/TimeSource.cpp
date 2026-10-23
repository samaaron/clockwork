// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * TimeSource.cpp — the audio thread's time, one shape on every target
 * (TimeSource.h). The one difference between targets is where an app thread
 * reads a wall clock: native has one; the worklet has only what the audio
 * thread last said.
 */
#include "clock/TimeSource.h"
#include "clock/clock_math.h"
#include "clockwork_config.h"   // clockwork_log

#include <algorithm>
#include <cstdint>

using clockwork::bitsToDouble;
using clockwork::doubleToBits;

double TimeSource::now() const {
    const uint64_t bits = mCurrentAudioThreadNTPBits.load(std::memory_order_acquire);
#if CLOCKWORK_WORKLET_CLOCK
    return bits ? bitsToDouble(bits) : 0.0;
#else
    return bits ? bitsToDouble(bits) : wall(wallClockNTP()) + globalOffsetSeconds();
#endif
}

double TimeSource::wallNow() const {
#if CLOCKWORK_WORKLET_CLOCK
    return now();
#else
    return wall(wallClockNTP()) + globalOffsetSeconds();
#endif
}

// The median of the residuals an anchor's settling window gathered: a copy
// on the stack, partially sorted — bounded work, no allocation, on the audio
// thread once an anchor.
double TimeSource::settledResidual() const {
    double sorted[clockwork::kAnchorSettleBlocks];
    std::copy(std::begin(mSettleResidual), std::end(mSettleResidual), std::begin(sorted));
    double* const mid = std::begin(sorted) + clockwork::kAnchorSettleBlocks / 2;
    std::nth_element(std::begin(sorted), mid, std::end(sorted));
    return *mid;
}

double TimeSource::updateAudioThreadNTP(double samplePosition, double sampleRate, double wallNtp) {
    const double sampleOffsetSec = samplePosition / sampleRate;
    const double global = globalOffsetSeconds();

    // Freewheel: pure sample-derived NTP, no wall-clock drift IIR. The headless
    // driver thread can be preempted on a busy machine; chasing that as "drift"
    // injects scheduling jitter a real device callback never sees. Deterministic
    // for offline/accuracy tests.
    if (mFreewheelClock.load(std::memory_order_relaxed)) {
        const double ntp = bitsToDouble(mBaseNTPBits.load(std::memory_order_relaxed)) + sampleOffsetSec + global;
        mCurrentAudioThreadNTPBits.store(doubleToBits(ntp), std::memory_order_release);
        return ntp;
    }

    //   sampleNTP = mBaseNTP + samplePosition / sampleRate
    //   drift     = wallNow - sampleNTP
    //   mBaseNTP += drift * 0.01   (low-pass converge ~1% per call)
    //   result    = mBaseNTP + samplePosition / sampleRate
    const double wallNow = wall(wallNtp);
    const double baseNTP = bitsToDouble(mBaseNTPBits.load(std::memory_order_relaxed));
    const double drift = wallNow - (baseNTP + sampleOffsetSec);

    // Surface timebase disturbances (wall-clock step/slew, callback stall):
    // a step is said at once; a persisting condition at most once a minute
    // (DriftLogGate). Not while an anchor settles: its first reading is
    // expected to be off, and the median takes that up. clockwork_log is a
    // lock-free egress-ring write.
    const bool settled = mSettleCount >= clockwork::kAnchorSettleBlocks;
    if (settled && mDriftLogGate.offer(drift, wallNow))
        clockwork_log("[clock] wall clock and audio clock have diverged by %+.1f ms", drift * 1000.0);

    // What this block moves the clock against its samples by. An anchor
    // settling (kAnchorSettleBlocks): nothing, until the last block of the
    // window moves it once to the median of the window's readings — one
    // reading is out by up to a render burst where the host reads Date.now(),
    // the median of sixty-four within a millisecond, and a callback that
    // woke late is one vote. On, only: an anchor that read late (the clock
    // ahead of the wall; a new worklet going on from the floor the arena
    // carries) is taken up at the bound like any residual, as time never goes
    // back. Settled: the IIR's pull, bounded (kClockSlewPerBlock) — the clock
    // is the ruler everything is scheduled on, and a ruler that flexes under
    // what is already on it misplaces it. Real drift is well under the bound
    // and followed in full; a wall clock set back is followed the same way;
    // and as the bound is far under a block a reader never sees time go
    // back, whatever the wall did.
    double pull = 0.0;
    if (!settled) {
        mSettleResidual[mSettleCount++] = drift;
        if (mSettleCount == clockwork::kAnchorSettleBlocks) pull = std::max(0.0, settledResidual());
    } else {
        pull = std::clamp(drift * clockwork::kDriftIirGain,
                          -clockwork::kClockSlewPerBlock, clockwork::kClockSlewPerBlock);
    }
    const double newBaseNTP = baseNTP + pull;
    const double ntp = newBaseNTP + sampleOffsetSec + global;
    // The clock against the wall, as the metrics show it: smoothed as the
    // clock itself is (one block's reading swings by a buffer where a browser
    // renders its quanta in bursts; the mean is what the clock follows), and
    // saturated into an int32 of microseconds.
    mDriftSmoothedSec += (-drift - mDriftSmoothedSec) * clockwork::kDriftIirGain;
    const double driftUs = mDriftSmoothedSec * 1e6;
    mLastDriftUs.store(driftUs > 2147483647.0 ? INT32_MAX : driftUs < -2147483648.0 ? INT32_MIN : static_cast<int32_t>(driftUs),
                       std::memory_order_relaxed);
    mBaseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mCurrentAudioThreadNTPBits.store(doubleToBits(ntp), std::memory_order_release);
    return ntp;
}

void TimeSource::resetAudioThreadTime(double samplePosition, double sampleRate, double wallNtp,
                                      double renderedFrames, double floorNtp) {
    const double sampleOffsetSec = samplePosition / sampleRate;
    // A freewheel clock from an origin is the frames rendered since it, with
    // no wall clock in it at all: the first start puts frame 0 at the origin,
    // and every later one goes on from where the rendering ended (`renderedFrames`
    // on the old count), so a restart's frame 0 is the next instant, never the
    // origin again — time going back would hold everything scheduled.
    const uint64_t originBits = mFreewheelOriginBits.load(std::memory_order_relaxed);
    double newBaseNTP = 0.0;
    if (mFreewheelClock.load(std::memory_order_relaxed) && originBits != 0) {
        newBaseNTP = mFreewheelAnchored.exchange(true, std::memory_order_relaxed)
            ? bitsToDouble(mBaseNTPBits.load(std::memory_order_relaxed))
                  + renderedFrames / sampleRate - sampleOffsetSec
            : bitsToDouble(originBits);
    } else {
        newBaseNTP = wall(wallNtp) - sampleOffsetSec;
        // Never behind the clock the arena already carries: a host whose wall
        // clock lags what the last host published (a worklet made again over
        // the same memory, an embedded host attaching again, a wall clock
        // stepped back) goes on from it, and a reader never sees time go back.
        if (floorNtp > 0.0 && newBaseNTP + sampleOffsetSec < floorNtp)
            newBaseNTP = floorNtp - sampleOffsetSec;
    }
    mBaseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mCurrentAudioThreadNTPBits.store(doubleToBits(newBaseNTP + sampleOffsetSec + globalOffsetSeconds()),
                                     std::memory_order_release);
    mDriftSmoothedSec = 0.0;
    mSettleCount = 0;   // the anchor settles over the blocks to come (updateAudioThreadNTP)
    mLastDriftUs.store(0, std::memory_order_relaxed);
    mAnchors.fetch_add(1, std::memory_order_relaxed);
}

void TimeSource::setFreewheelClock(bool enabled, double originNtp) {
    mFreewheelOriginBits.store(doubleToBits(originNtp), std::memory_order_relaxed);
    mFreewheelAnchored.store(false, std::memory_order_relaxed);
    mFreewheelClock.store(enabled, std::memory_order_relaxed);
}

bool TimeSource::freewheelClock() const {
    return mFreewheelClock.load(std::memory_order_relaxed);
}
