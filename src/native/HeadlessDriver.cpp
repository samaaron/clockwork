// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * HeadlessDriver.cpp — the timer loop, and the system pacer for each platform.
 */
#include "HeadlessDriver.h"
#include "ClockworkProcessor.h"
#include "RealtimeThread.h"
#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"
#include "native/LinkAudioHost.h"
#include "lanes/lanes.h"

#include <algorithm>
#include <cstdint>

#if defined(__linux__)
  #include <time.h>
#elif defined(__APPLE__)
  #include <mach/mach_time.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

// ── The system pacer ─────────────────────────────────────────────────────────
// The platform's highest-resolution timer, in nanoseconds, and the real wall
// clocks. Its sleeps do not hear a stop request: the loop checks after each.

namespace {

#if defined(__linux__)

int64_t timespecToNs(const struct timespec& ts) {
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

class SystemPacer final : public HeadlessPacer {
public:
    int64_t nowNs() override {
        struct timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return timespecToNs(now);
    }
    void sleepUntilNs(int64_t deadlineNs, const std::stop_token&) override {
        struct timespec until{};
        until.tv_sec  = static_cast<time_t>(deadlineNs / 1'000'000'000LL);
        until.tv_nsec = static_cast<long>(deadlineNs % 1'000'000'000LL);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr);
    }
    double wallNtp() override { return wallClockNTP(); }
    double monotonicSeconds() override { return clockwork::monotonicSeconds(); }
};

#elif defined(__APPLE__)

class SystemPacer final : public HeadlessPacer {
public:
    SystemPacer() { mach_timebase_info(&mTimebase); }
    int64_t nowNs() override { return toNs(mach_absolute_time()); }
    void sleepUntilNs(int64_t deadlineNs, const std::stop_token&) override {
        mach_wait_until(toTicks(deadlineNs));
    }
    double wallNtp() override { return wallClockNTP(); }
    double monotonicSeconds() override { return clockwork::monotonicSeconds(); }

private:
    int64_t toNs(uint64_t ticks) const {
        return static_cast<int64_t>(ticks * mTimebase.numer / mTimebase.denom);
    }
    uint64_t toTicks(int64_t ns) const {
        return static_cast<uint64_t>(ns) * mTimebase.denom / mTimebase.numer;
    }
    mach_timebase_info_data_t mTimebase{};
};

#elif defined(_WIN32)

class SystemPacer final : public HeadlessPacer {
public:
    SystemPacer() {
        QueryPerformanceFrequency(&mFrequency);
        mTimer = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!mTimer) mTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~SystemPacer() override {
        if (mTimer) CloseHandle(mTimer);
    }
    int64_t nowNs() override {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart * 1'000'000'000LL / mFrequency.QuadPart;
    }
    void sleepUntilNs(int64_t deadlineNs, const std::stop_token&) override {
        const int64_t remaining = deadlineNs - nowNs();
        if (remaining <= 0 || !mTimer) return;
        LARGE_INTEGER due;
        due.QuadPart = -(remaining / 100);   // 100 ns units, relative
        SetWaitableTimer(mTimer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(mTimer, INFINITE);
    }
    double wallNtp() override { return wallClockNTP(); }
    double monotonicSeconds() override { return clockwork::monotonicSeconds(); }

private:
    LARGE_INTEGER mFrequency{};
    HANDLE        mTimer = nullptr;
};

#endif

} // namespace

std::shared_ptr<HeadlessPacer> systemPacer() {
    return std::make_shared<SystemPacer>();
}

// ── The driver ───────────────────────────────────────────────────────────────

void HeadlessDriver::configure(ClockworkProcessor* callback,
                                int sampleRate,
                                int blockSize,
                                int numOutputChannels,
                                int numInputChannels) {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    mCallback          = callback;
    mSampleRate        = (sampleRate > 0) ? sampleRate : 48000;
    mBlockSize         = (blockSize  > 0) ? blockSize  : 128;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;
}

void HeadlessDriver::setClockworkClock(ClockworkClock* sc) {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    mClockworkClock = sc;
}

void HeadlessDriver::setLinkAudio(LinkAudioHost* la) {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    mLinkAudio = la;
}

void HeadlessDriver::setMetrics(PerformanceMetrics* m) {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    mMetrics = m;
}

void HeadlessDriver::setPacer(std::shared_ptr<HeadlessPacer> pacer) {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    mPacer = std::move(pacer);
}

void HeadlessDriver::start() {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    if (mThread.joinable()) return;
    if (!mPacer) mPacer = systemPacer();
    mThread = std::jthread([this](const std::stop_token& stop) {
        // Scheduled as a device's callback thread is: the headless driver is
        // the engine's audio clock, and late blocks are late audio.
        clockwork::elevateCurrentThreadToRealtime(
            static_cast<double>(mBlockSize) / static_cast<double>(mSampleRate));
        run(stop);
    });
}

void HeadlessDriver::stop() {
    const std::lock_guard<std::mutex> lock(mLifecycle);
    if (!mThread.joinable()) return;
    mThread.request_stop();
    mThread.join();
}

// Shared per-block loop body — derives NTP via ClockworkClock, ticks the engine
// through the lanes ABI, publishes the block, and wakes workers.
void HeadlessDriver::processBlock(double& samplePos, HeadlessPacer& pacer) {
    // Headless has no DAC, so "audible" == render time (latency 0). One Link
    // Audio stamp per block, passed to both drain and publish: two stamps
    // with process_audio between would let the published stream's beat-time
    // advance at process_audio cost rather than sample rate.
    ClockworkBlockTime time {};
    time.audio_frames   = static_cast<uint64_t>(samplePos);
    time.wall_ntp       = pacer.wallNtp();
    time.monotonic_secs = pacer.monotonicSeconds();
    renderAudioBlock(*mLinkAudio,
                     static_cast<uint32_t>(mBlockSize),
                     static_cast<uint32_t>(mNumOutputChannels),
                     static_cast<uint32_t>(mNumInputChannels),
                     static_cast<uint32_t>(mSampleRate),
                     time);
    samplePos += mBlockSize;

    mCallback->processCount.fetch_add(1, std::memory_order_release);
    mCallback->processCount.notify_all();
}

int64_t HeadlessDriver::cappedNextWake(int64_t nextWakeNs, int64_t nowNs) {
    constexpr auto kMaxLagNs = static_cast<int64_t>(clockwork::kClockStepSeconds * 1e9);
    if (nowNs - nextWakeNs > kMaxLagNs)
        return nowNs;   // a gap: the backlog is dropped and the clock steps
    return nextWakeNs;
}

void HeadlessDriver::run(const std::stop_token& stop) {
    HeadlessPacer& pacer = *mPacer;
    const int64_t blockNs = 1'000'000'000LL * mBlockSize / mSampleRate;
    int64_t nextWake = pacer.nowNs();
    double samplePos = 0.0;
    clockwork_audio_starts(pacer.wallNtp(), pacer.monotonicSeconds());   // this driver's audio starts now

    while (!stop.stop_requested()) {
        processBlock(samplePos, pacer);
        nextWake = cappedNextWake(nextWake + blockNs, pacer.nowNs());
        pacer.sleepUntilNs(nextWake, stop);
    }
}
