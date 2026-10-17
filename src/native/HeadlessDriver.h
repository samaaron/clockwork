// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * HeadlessDriver.h — Timer-driven audio processing without a real audio device.
 *
 * When no audio hardware is available (headless mode), this thread calls
 * process_audio() at the correct block rate (matches whatever block size
 * the engine configured, typically 128 samples), using platform-specific
 * high-resolution timers for accurate timing:
 *
 *   Linux:   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)
 *   macOS:   mach_wait_until()
 *   Windows: WaitableTimer with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 *
 * Worker threads (ReplyReader, DebugReader) are woken after each block,
 * so OSC replies flow exactly as they would with a real audio device.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>

class ClockworkProcessor;
class ClockworkClock;
class LinkAudioHost;
struct PerformanceMetrics;

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

    // Tick on a thread of its own, scheduled as an audio thread is at its
    // block period, until stop(). Idempotent.
    void start();
    // Stop ticking: the block in hand finishes, then the thread is joined.
    // Idempotent.
    void stop();

    // Bound on how far the timer loop replays missed blocks after a scheduling
    // gap (thread starvation, sleep/wake). Beyond this the loop re-anchors to
    // the current time and drops the backlog rather than firing a back-to-back
    // "catch-up" burst of process_audio() calls. ~8 blocks (~21 ms at 128/48k)
    // is far beyond normal timer jitter (<1 block) yet caps a stampede hard.
    static constexpr int kMaxCatchupBlocks = 8;

    // Given the just-advanced wake deadline, the current time, and the block
    // period — all in the SAME tick unit (ns, mach ticks, or QPC ticks) —
    // return the deadline to actually sleep until. If the loop has fallen more
    // than kMaxCatchupBlocks behind `now`, re-anchor to `now` (drop backlog);
    // otherwise keep the scheduled deadline so small lags realign smoothly.
    // Pure and unit-agnostic so the anti-stampede rule is unit-testable without
    // real timing. Sample position is unaffected — only the wall-clock deadline
    // moves, so audio stays sample-continuous; it just stops sprinting.
    static int64_t cappedNextWake(int64_t nextWake, int64_t now,
                                  int64_t blockTicks);

private:
    // The platform's high-resolution timer loop, until `stop` is requested.
    void run(const std::stop_token& stop);

    // Shared loop body: derive NTP via ClockworkClock,
    // process audio, wake workers. Called once per block.
    void processBlock(double& samplePos);

    // Ticks the engine at this block size. Set explicitly from
    // ClockworkEngine::Config so the tick rate is deterministic and
    // independent of whatever buffer size a transient real device may
    // have left on the audio callback (e.g. failed-init fallback path).
    int mBlockSize = 128;

    ClockworkProcessor* mCallback         = nullptr;
    ClockworkClock*        mClockworkClock       = nullptr;
    LinkAudioHost*   mLinkAudio      = nullptr;
    PerformanceMetrics* mMetrics     = nullptr;
    int                mSampleRate        = 48000;
    int                mNumOutputChannels = 2;
    int                mNumInputChannels  = 0;

    // Held by configure, the setters, start and stop: what one thread set up
    // and stopped, another starts, and sees all of it.
    std::mutex         mLifecycle;
    std::jthread       mThread;
};
