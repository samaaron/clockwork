// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * TimeSource.cpp — audio-thread time-source. Two shapes, one guard
 * (CLOCKWORK_WORKLET_CLOCK), matching TimeSource.h.
 *
 * native (WallClock + IIR): now() / nowAt() / updateAudioThreadNTP() are
 * audio-thread reads: concrete (no vtable), atomics only. updateAudioThreadNTP
 * runs one low-pass step per callback, converging the sample-derived clock
 * toward the wall clock at ~1% per call; freewheel mode bypasses that and
 * returns a pure sample-derived NTP.
 *
 * worklet (self-driven): nowAt() evaluates the SAB time formula from the bound
 * offset pointers and publishes it to the now()-cache — no wall clock, no IIR,
 * no freewheel.
 */
#include "clock/TimeSource.h"
#include "clock/clock_math.h"

using clockwork::doubleToBits;
using clockwork::bitsToDouble;

extern "C" int clockwork_log(const char* fmt, ...);

#if CLOCKWORK_WORKLET_CLOCK

double TimeSource::now() const {
    return bitsToDouble(mLastAudioThreadNTPBits.load(std::memory_order_acquire));
}

double TimeSource::nowAt(double audioCurrentTime) const {
    // The two offsets are integers and are almost always zero: the drift
    // offset moves only when the host resyncs after a suspend, the global
    // offset only when an embedder shifts the whole clock. Both conversions
    // to seconds cost an int→double widening and a soft-float divide on a
    // target with no double FPU (the ESP32-S3 runs both through ROM
    // __floatsidf/__divdf3, ~500 cycles a block for two answers that are
    // both zero), so the zero case is answered without arithmetic.
    // Exactly equal, not an approximation: 0 / 1000000.0 IS 0.0. The loads
    // themselves are unchanged — the acquire on the drift offset still
    // orders this block's read of it.
    const int32_t drift_us =
        mDriftOffset ? mDriftOffset->load(std::memory_order_acquire) : 0;
    const double drift_seconds = drift_us ? (drift_us / 1000000.0) : 0.0;
    const double ntp_start =
        (mNtpStartTime && *mNtpStartTime != 0.0) ? *mNtpStartTime : 0.0;
    const int32_t global_ms =
        mGlobalOffset ? mGlobalOffset->load(std::memory_order_relaxed) : 0;
    const double global_seconds = global_ms ? (global_ms / 1000.0) : 0.0;
    const double result = audioCurrentTime + ntp_start + drift_seconds + global_seconds;
    mLastAudioThreadNTPBits.store(doubleToBits(result), std::memory_order_release);
    return result;
}

double TimeSource::wallNow() const {
    // No wall clock on the worklet; the last audio-thread NTP is the best
    // available "right now" (0.0 only before the first nowAt()). Callers that
    // stamp events with wallNow() (e.g. /clockwork/clock/transport/set) would otherwise
    // record NTP epoch 1900.
    return now();
}

double TimeSource::updateAudioThreadNTP(double samplePosition,
                                        double sampleRate,
                                        double audioCurrentTime) {
    (void)samplePosition;
    (void)sampleRate;
    return nowAt(audioCurrentTime);
}

void TimeSource::resetAudioThreadTime(double samplePosition, double sampleRate) {
    (void)samplePosition;
    (void)sampleRate;
}

void TimeSource::setFreewheelClock(bool enabled) {
    // No-op: the worklet evaluates the SAB time formula in nowAt() and has no
    // headless driver / drift IIR to bypass.
    (void)enabled;
}

#else  // !CLOCKWORK_WORKLET_CLOCK

double TimeSource::now() const {
    const uint64_t bits =
        mCurrentAudioThreadNTPBits.load(std::memory_order_acquire);
    if (bits == 0) return readWallClock();
    return bitsToDouble(bits);
}

double TimeSource::nowAt(double audioCurrentTime) const {
    (void)audioCurrentTime;  // Native uses its own time source.
    return now();
}

double TimeSource::wallNow() const {
    return readWallClock();
}

double TimeSource::updateAudioThreadNTP(double samplePosition,
                                        double sampleRate,
                                        double audioCurrentTime) {
    (void)audioCurrentTime;
    const double sampleOffsetSec = samplePosition / sampleRate;

    // Freewheel: pure sample-derived NTP, no wall-clock drift IIR. The headless
    // driver thread can be preempted on a busy machine; chasing that as "drift"
    // injects scheduling jitter a real device callback never sees. Deterministic
    // for offline/accuracy tests.
    if (mFreewheelClock.load(std::memory_order_relaxed)) {
        const double wallNTP =
            bitsToDouble(mBaseNTPBits.load(std::memory_order_relaxed))
            + sampleOffsetSec;
        mCurrentAudioThreadNTPBits.store(doubleToBits(wallNTP),
                                         std::memory_order_release);
        return wallNTP;
    }

    //   sampleNTP = mBaseNTP + samplePosition / sampleRate
    //   drift     = wallNow - sampleNTP
    //   mBaseNTP += drift * 0.01   (low-pass converge ~1% per call)
    //   result    = mBaseNTP + samplePosition / sampleRate
    const double wallNow = readWallClock();
    const double baseNTP = bitsToDouble(
        mBaseNTPBits.load(std::memory_order_relaxed));
    const double drift = wallNow - (baseNTP + sampleOffsetSec);

    // Surface timebase disturbances (wall-clock step/slew, callback stall):
    // a genuine step is reported immediately; jitter that *hovers* at the
    // threshold (VMs, Windows shared-mode) summarises at most once per
    // minute instead of becoming per-second wallpaper — see DriftLogGate.
    // clockwork_log is a lock-free egress-ring write.
    clockwork::DriftLogGate::Line line;
    if (mDriftLogGate.offer(drift, wallNow, line)) {
        clockwork_log("DRIFT: wall clock %+.1fms from audio timebase, re-converging "
               "(peak %.1fms, %u hits since last report, %u total)",
               line.driftSec * 1000.0, line.peakSec * 1000.0,
               line.hits, line.total);
    }

    const double newBaseNTP = baseNTP + drift * clockwork::kDriftIirGain;
    const double wallNTP = newBaseNTP + sampleOffsetSec;
    mBaseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mCurrentAudioThreadNTPBits.store(doubleToBits(wallNTP),
                                     std::memory_order_release);
    return wallNTP;
}

void TimeSource::resetAudioThreadTime(double samplePosition, double sampleRate) {
    const double sampleOffsetSec = samplePosition / sampleRate;
    const double newBaseNTP = readWallClock() - sampleOffsetSec;
    mBaseNTPBits.store(doubleToBits(newBaseNTP), std::memory_order_relaxed);
    mCurrentAudioThreadNTPBits.store(
        doubleToBits(newBaseNTP + sampleOffsetSec),
        std::memory_order_release);
}

void TimeSource::setFreewheelClock(bool enabled) {
    mFreewheelClock.store(enabled, std::memory_order_relaxed);
}

#endif  // CLOCKWORK_WORKLET_CLOCK
