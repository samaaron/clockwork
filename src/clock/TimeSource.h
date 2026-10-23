// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * TimeSource.h — the audio thread's time, one shape on every target.
 *
 * The monotonic sample counter, anchored to a wall clock: each block runs one
 * IIR step (updateAudioThreadNTP) toward the wall time the HOST hands over
 * with the block — native's callback reads its own wall clock, the worklet
 * passes Date.now(), an embedded host its own — and now() returns the latest
 * value. The clock is the ruler everything is scheduled on, so the IIR's pull
 * is bounded (kClockSlewPerBlock, a few µs a block: more than crystals drift
 * by) and what is on the ruler stays where its sender put it. An anchor is
 * one reading, out by up to a render burst where the host reads Date.now(),
 * so it settles: the clock holds the reading for kAnchorSettleBlocks, then
 * moves once to the median of the readings meanwhile, and the bound holds it
 * from there. The clock never steps on a wall reading: a wall set forward is
 * followed at the bound, as one set back is. A gap — no block rendered while
 * time passed: a stall, a context suspended, a sleep, a reload — is the
 * host's to see, against the audio it rendered (clockwork_audio_back), and it
 * anchors the clock afresh at the block back (resetAudioThreadTime), since a
 * clock that converges from seconds behind runs fast until it has caught up
 * and everything scheduled against it plays compressed. The audio thread
 * anchors itself on its first block, and a host at its audio start and after
 * a gap; nothing else writes its time, so
 * no block ever reads an anchor someone else is half-way through rewriting
 * (the page used to write an NTP start and a drift into the arena, in steps,
 * and a new worklet over the same memory published whatever mix was there:
 * the clock stepped back 0.7 s in Clockwork's own test and 13.7 s under Sonic
 * Pi, 2026-10-23). Time never goes back: a reset has a floor, the clock the
 * arena already carries (ClockworkClock::floorRenderNtp), and the bound is far
 * under a block.
 *
 * Freewheel mode bypasses the drift IIR and derives NTP purely from sample
 * position (deterministic for offline / accuracy tests).
 *
 * The host's global offset (the arena's clock-anchors region, a deliberate
 * shift for multi-system sync) is added to every answer, on every target.
 *
 * Depends only on the centralised double↔uint64 bit-cast — no JUCE, no
 * Ableton, no guest DSP. The Link-clock-domain block stamp
 * (LinkAudioHost::blockHostMicros) is the Link Audio host's own anchor, not
 * here. Dispatch is link-time-concrete (no vtable) so the audio-thread reads
 * stay inlinable and branch-free.
 */
#pragma once

#include "shared_memory.h"
#include "clock/clock_math.h"

#include <atomic>
#include <cstdint>

namespace clockwork {

// Rate-gates the wall-clock drift diagnostic. Steady-state jitter on VMs
// and Windows shared-mode WASAPI hovers right at the threshold, so a
// per-second limit still logs forever (thousands of lines per session);
// ASIO-grade callbacks stay below it and log nothing. A genuine step is
// reported immediately; a persisting condition at most once per minute.
// Audio-thread only — plain fields.
struct DriftLogGate {
    static constexpr double kThresholdSec   = 0.005;
    static constexpr double kMinIntervalSec = 60.0;

    // Returns true when a line should be emitted.
    bool offer(double driftSec, double wallNow) {
        const double mag = driftSec < 0.0 ? -driftSec : driftSec;
        if (mag <= kThresholdSec || wallNow - mLastLogNTP < kMinIntervalSec)
            return false;
        mLastLogNTP = wallNow;
        ++mReports;
        return true;
    }
    // Lines emitted so far.
    uint32_t reports() const { return mReports; }

private:
    double   mLastLogNTP{0.0};
    uint32_t mReports{0};
};

} // namespace clockwork

class TimeSource {
public:
    TimeSource() = default;
    TimeSource(const TimeSource&) = delete;
    TimeSource& operator=(const TimeSource&) = delete;
    ~TimeSource() = default;

    // The host's global offset word in the arena (milliseconds), added to
    // every answer. Bound as the clock enters service, before the audio
    // thread runs; null is 0.
    void bindGlobalOffset(const std::atomic<int32_t>* globalOffset) { mGlobalOffset = globalOffset; }

    // App-thread read: the latest audio-thread NTP. Before the first block,
    // the wall clock where there is one (native); 0 on the worklet.
    double now() const;
    // App-thread wall-clock NTP. The worklet has no wall clock of its own
    // and answers the last audio-thread NTP (0 before the first block).
    double wallNow() const;

    // Audio-thread time-base: one IIR step per block toward `wallNtp`, the
    // host's wall clock as it hands the block over, returning the NTP for
    // this block, never below the last block's (or the pure sample-derived
    // NTP in freewheel mode).
    double updateAudioThreadNTP(double samplePosition, double sampleRate, double wallNtp);
    // Anchor afresh at `wallNtp`, and never below `floorNtp` (0: no floor):
    // the clock the arena already carries, so a new host over memory that is
    // up goes on from it. `renderedFrames`: the frames rendered so far on the
    // count being left, where a freewheel clock from an origin goes on from.
    void   resetAudioThreadTime(double samplePosition, double sampleRate, double wallNtp,
                                double renderedFrames = 0.0, double floorNtp = 0.0);
    // Whether a reset has anchored this clock yet.
    bool   anchored() const { return mBaseNTPBits.load(std::memory_order_relaxed) != 0; }
    // For the metrics (publishClockMetrics): the clock against the host's
    // wall clock at the last block, microseconds, signed; and how many times
    // it has been anchored afresh since boot.
    int32_t  lastDriftUs() const { return mLastDriftUs.load(std::memory_order_relaxed); }
    uint32_t anchors() const { return mAnchors.load(std::memory_order_relaxed); }
    // Lines the drift diagnostic has logged (audio-thread read; tests).
    uint32_t driftReports() const { return mDriftLogGate.reports(); }

    void   setFreewheelClock(bool enabled, double originNtp = 0.0);
    bool   freewheelClock() const;

    // Test boundary: when non-zero, the IIR reads this instead of the wall
    // time the host passes, so scenario tests (test_time_source.cpp) can
    // drive exact stall / rate-mismatch shapes against exact expected
    // answers. Costs one relaxed atomic load on the audio path; zero (the
    // default) = the host's.
    void setTestWallClock(double ntpSeconds) {
        mTestWallBits.store(clockwork::doubleToBits(ntpSeconds), std::memory_order_relaxed);
    }

private:
    double settledResidual() const;   // the median of an anchor's settling window
    double wall(double hostWallNtp) const {
        const uint64_t bits = mTestWallBits.load(std::memory_order_relaxed);
        return bits ? clockwork::bitsToDouble(bits) : hostWallNtp;
    }
    double globalOffsetSeconds() const {
        const int32_t ms = mGlobalOffset ? mGlobalOffset->load(std::memory_order_relaxed) : 0;
        return ms ? (ms / 1000.0) : 0.0;   // exactly 0 without arithmetic, the common case
    }

    const std::atomic<int32_t>* mGlobalOffset{nullptr};
    std::atomic<uint64_t> mTestWallBits{0};
    std::atomic<int32_t>  mLastDriftUs{0};
    std::atomic<uint32_t> mAnchors{0};
    double                mDriftSmoothedSec{0.0};   // audio thread only
    // An anchor settling (kAnchorSettleBlocks): the residual at each block
    // since it, and how many so far. Audio thread only.
    double                mSettleResidual[clockwork::kAnchorSettleBlocks]{};
    uint32_t              mSettleCount{clockwork::kAnchorSettleBlocks};
    // IIR-smoothed audio-thread NTP. Bit-pattern in uint64 because
    // std::atomic<double> isn't lock-free everywhere.
    std::atomic<uint64_t> mBaseNTPBits{0};
    std::atomic<uint64_t> mCurrentAudioThreadNTPBits{0};
    // When true, updateAudioThreadNTP skips the wall-clock drift IIR and
    // returns a pure sample-derived NTP (deterministic; see setFreewheelClock).
    std::atomic<bool>     mFreewheelClock{false};
    // With it, the NTP frame 0 is heard at, in place of the wall clock at a
    // reset (0: the wall clock); and whether the clock has started from it,
    // after which a reset goes on from where the rendering ended.
    std::atomic<uint64_t> mFreewheelOriginBits{0};
    std::atomic<bool>     mFreewheelAnchored{false};
    // Drift-step logging state (audio-thread only).
    clockwork::DriftLogGate mDriftLogGate;
};
