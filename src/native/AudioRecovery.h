// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#pragma once

#include <cstdint>

// Pure, unit-agnostic audio liveness/recovery logic — the authoritative answer
// to "is the audio thread actually alive, and if not, what recovery step are we
// on". Kept free of JUCE/CoreAudio and of any real clock so the rules are
// unit-testable without hardware or timing, mirroring DevicePolicy and
// HeadlessDriver::cappedNextWake. The engine feeds it processCount samples and a
// monotonic `now` (any integer unit) and acts on its verdicts; it owns no state
// the engine also owns.
namespace clockwork::audio {

// Derived purely from the audio-thread tick counter (processCount) sampled over
// time. The key distinction: a single resumed tick ("twitch") is Confirming,
// NOT Live — liveness requires ticks SUSTAINED across the confirm window, so a
// device that emits one callback per reopen attempt can never masquerade as
// recovered.
enum class LivenessPhase {
    Live,        // ticks have been sustained for >= the confirm window
    Confirming,  // ticks resumed after a stall but not yet sustained
    Stalled,     // no tick for >= the stall window
};

class LivenessMonitor {
public:
    // stallWindow: no-tick duration that counts as a stall.
    // confirmWindow: how long ticks must keep advancing, after a stall, before
    // the audio is trusted as Live again. Same integer unit as `now`.
    LivenessMonitor(int64_t stallWindow, int64_t confirmWindow);

    // Feed the current audio tick counter sampled at time `now`.
    void observe(uint64_t tickCount, int64_t now);

    // Current phase at time `now`. Pure query — Stalled can be reached by time
    // passing with no new observation.
    LivenessPhase phase(int64_t now) const;

private:
    int64_t  mStallWindow;
    int64_t  mConfirmWindow;
    uint64_t mLastCount   = 0;
    int64_t  mLastAdvance = 0;   // time tickCount last increased
    int64_t  mRunStart    = 0;   // time the current uninterrupted advancing run began
    bool     mSeen        = false;
};

// Detects a device whose callbacks keep ticking (LivenessMonitor reads Live)
// but deliver samples at the wrong rate — the post-sleep DirectSound failure
// where the emulation timer free-runs fast or slow. The clock IIR
// (TimeSource::updateAudioThreadNTP) then parks at a permanent equilibrium
// offset between wall clock and audio timebase (drift where correction rate
// equals inflow), which no NTP re-anchor can converge; only reopening the
// device restores the rate.
//
// Feed cumulative rendered frames plus a monotonic `now`; every completed
// window yields a delivered/nominal ratio, and only N CONSECUTIVE
// out-of-tolerance windows produce a verdict. One window is never enough: a
// single transient callback stall (the "[gap] audio callback stalled" case)
// skews that window's ratio and must not trigger a cold swap.
class RateSkewMonitor {
public:
    // window: measurement span per ratio. maxGap: observation gap that marks a
    // discontinuity (the caller stopped sampling — swap in flight, benign
    // states, machine asleep). tolerance: fractional deviation from nominal
    // that makes a window bad. badWindowsRequired: consecutive bad windows
    // before skewed() reports true. window/maxGap in the same unit as `now`.
    RateSkewMonitor(int64_t window, int64_t maxGap, double tolerance,
                    int badWindowsRequired);

    // Feed cumulative rendered frames at time `now`, with the nominal device
    // rate in frames per time-unit. An observation gap > maxGap, a frames
    // rollback (device restart resets the counter) or a nominal-rate change
    // discards the current window AND the bad streak — each marks a
    // discontinuity the verdict must restart from.
    void observe(uint64_t frames, double nominalFramesPerUnit, int64_t now);

    bool   skewed()    const { return mBadStreak >= mBadWindowsRequired; }
    // delivered/nominal of the last completed window (1.0 before the first).
    double lastRatio() const { return mLastRatio; }
    // Windows completed over the monitor's whole life (reset() does not
    // clear it), and whether the latest of them was within tolerance. A
    // caller watching for "the device has come back healthy" compares the
    // count it last saw and asks about the newest window — the verdict
    // alone cannot say that, since skewed() is also false mid-window.
    uint64_t windowsCompleted() const { return mWindowsCompleted; }
    bool     lastWindowGood()   const { return mLastWindowGood; }
    void   reset();

private:
    int64_t mWindow;
    int64_t mMaxGap;
    double  mTolerance;
    int     mBadWindowsRequired;

    bool     mSeen        = false;
    uint64_t mStartFrames = 0;   // window anchor
    int64_t  mStartTime   = 0;
    uint64_t mLastFrames  = 0;   // previous observation (discontinuity checks)
    int64_t  mLastTime    = 0;
    double   mRate        = 0.0; // nominal frames-per-unit the window was anchored with
    int      mBadStreak   = 0;
    double   mLastRatio   = 1.0;
    uint64_t mWindowsCompleted = 0;
    bool     mLastWindowGood   = true;
};

// What to do about a skew verdict that keeps coming back. The monitor above
// measures; this decides the remedy, and it exists because the obvious
// remedy — reopen the device — is the wrong tool for one whole class of
// fault. A device clocked at 44.1 kHz that reports 48 kHz (a USB mixer
// through Apple's class driver, sonic-pi#3565) delivers 0.919x real time
// forever: reopening it at the nominal rate reproduces the exact condition
// that triggered the reopen, and the watchdog measures it again ten seconds
// later. Left alone that is a storm — 79 cold swaps in one session, each one
// stopping the client's jobs and re-initialising its world.
//
// So: a skew that is TRANSIENT (a post-sleep timer free-run) is fixed by a
// reopen, and gets one. A skew that comes back with the SAME ratio after
// each reopen is a property of the device, and after `maxRecoveries` of them
// the remedy changes: the next swap asks for the rate the device is actually
// delivering (ratio x nominal), which is the only rate it can keep time at.
// If it skews at that rate too, the device is lying in a way no rate request
// corrects, and the policy gives up for the session: the engine keeps
// playing, off-pitch, and says so once. Never another swap.
enum class RateSkewAction {
    Recover,            // reopen at the nominal rate, as for a transient skew
    AdoptMeasuredRate,  // reopen asking for ratio x nominal
    GiveUp,             // stop recovering; say so once
    None,               // already given up: nothing to do
};

class RateSkewPolicy {
public:
    // maxRecoveries: consecutive same-ratio verdicts acted on before the
    // fault is called persistent (the Nth is the adopt). sameRatioTolerance:
    // two ratios closer than this are the same fault.
    RateSkewPolicy(int maxRecoveries, double sameRatioTolerance);

    // What acting on a verdict at `ratio` would do. Pure: the caller asks,
    // attempts the action (a recovery can be refused — in flight, cooling
    // down), and reports back with acted() only when it happened, so a
    // refused attempt cannot advance the streak.
    RateSkewAction next(double ratio) const;
    // The action next(ratio) named was carried out.
    void acted(double ratio);
    // A window came back within tolerance after a recovery: whatever the
    // fault was, it is gone, and the streak restarts from nothing. Not a
    // pardon for a policy that has given up — that is for the session.
    void healthy();

    int  streak() const { return mStreak; }
    bool adopted() const { return mPhase == Phase::Adopted; }
    bool gaveUp()  const { return mPhase == Phase::GaveUp; }

private:
    enum class Phase { Recovering, Adopted, GaveUp };
    bool sameRatio(double a, double b) const;

    int    mMaxRecoveries;
    double mTolerance;
    Phase  mPhase     = Phase::Recovering;
    int    mStreak    = 0;
    double mLastRatio = 0.0;
};

} // namespace clockwork::audio
