// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * HeadlessDriver.h — timer-driven audio processing without a real audio device.
 *
 * When no audio hardware is available (headless mode), a thread of its own
 * calls process_audio() at the block rate the engine configured, so the
 * engine has a clock and its control path drains as it would under a
 * device. Worker threads are woken after each block, so OSC replies flow
 * exactly as they would with a real audio device.
 *
 * The driver paces itself on a HeadlessPacer: what time it is, how to sleep
 * until a time, and the wall clocks each block is stamped with. The system
 * pacer is the platform's high-resolution timer (clock_nanosleep,
 * mach_wait_until, a high-resolution waitable timer); a test hands in a
 * pacer whose time moves only when the test moves it, and says exactly how
 * late every wake lands.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

class ClockworkProcessor;
class ClockworkClock;
class LinkAudioHost;
struct PerformanceMetrics;

// The driver's clock. Nanoseconds from any origin, never going back.
struct HeadlessPacer {
    virtual ~HeadlessPacer() = default;
    virtual int64_t nowNs() = 0;
    // Until `deadlineNs`, or until `stop` is requested if the pacer can
    // hear it. Returning early is allowed; the loop checks the time.
    virtual void sleepUntilNs(int64_t deadlineNs, const std::stop_token& stop) = 0;
    // The clocks a block is stamped with (ClockworkBlockTime): wall NTP, and
    // the monotonic clock that tells a gap from a wall clock set by hand.
    virtual double wallNtp() = 0;
    virtual double monotonicSeconds() = 0;
};

// The platform's timer and clocks.
std::shared_ptr<HeadlessPacer> systemPacer();

// Starting, stopping and setting it up are serialised: the engine's watchdog
// restarts a driver that another thread may have just stopped.
class HeadlessDriver {
public:
    HeadlessDriver() = default;
    ~HeadlessDriver() { stop(); }
    HeadlessDriver(const HeadlessDriver&) = delete;
    HeadlessDriver& operator=(const HeadlessDriver&) = delete;
    HeadlessDriver(HeadlessDriver&&) = delete;
    HeadlessDriver& operator=(HeadlessDriver&&) = delete;

    void configure(ClockworkProcessor* callback,
                   int sampleRate,
                   int blockSize,
                   int numOutputChannels,
                   int numInputChannels);

    // Set before start(), as configure is.
    void setClockworkClock(ClockworkClock* sc);
    void setLinkAudio(LinkAudioHost* la);
    // The engine's dashboard block, mirrored into per block; null skips it.
    void setMetrics(PerformanceMetrics* m);
    // What the driver paces on; null is the system pacer.
    void setPacer(std::shared_ptr<HeadlessPacer> pacer);

    // Tick on a thread of its own, scheduled as an audio thread is at its
    // block period, until stop(). Idempotent.
    void start();
    // Stop ticking: the block in hand finishes, then the thread is joined.
    // Idempotent.
    void stop();

    // Given the just-advanced wake deadline and the current time, both in
    // nanoseconds, the deadline to actually sleep until. A loop that fell
    // behind by less than the clock's gap threshold (kClockStepSeconds)
    // keeps its deadline and renders what it owes back to back: a starved
    // machine wakes it late, but time is not lost. Behind by more than
    // that, it re-anchors to now and drops the backlog: that is a sleep or
    // a thread held for a long time, and the clock steps for it as the gap
    // rule says, rather than replaying seconds of audio in a burst. Pure.
    // The sample position is unaffected either way; audio stays
    // sample-continuous.
    static int64_t cappedNextWake(int64_t nextWakeNs, int64_t nowNs);

private:
    // The timer loop on the pacer, until `stop` is requested.
    void run(const std::stop_token& stop);

    // Shared loop body: derive NTP via ClockworkClock,
    // process audio, wake workers. Called once per block.
    void processBlock(double& samplePos, HeadlessPacer& pacer);

    // Ticks the engine at this block size. Set explicitly from
    // ClockworkEngine::Config so the tick rate is deterministic and
    // independent of whatever buffer size a transient real device may
    // have left on the audio callback (e.g. failed-init fallback path).
    int mBlockSize = 128;

    ClockworkProcessor* mCallback         = nullptr;
    ClockworkClock*        mClockworkClock       = nullptr;
    LinkAudioHost*   mLinkAudio      = nullptr;
    PerformanceMetrics* mMetrics     = nullptr;
    std::shared_ptr<HeadlessPacer> mPacer;
    int                mSampleRate        = 48000;
    int                mNumOutputChannels = 2;
    int                mNumInputChannels  = 0;

    // Held by configure, the setters, start and stop: what one thread set up
    // and stopped, another starts, and sees all of it.
    std::mutex         mLifecycle;
    std::jthread       mThread;
};
