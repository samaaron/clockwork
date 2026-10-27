// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * FakePacer.h — the headless driver's clock, owned by the case.
 *
 * The headless driver paces itself on a HeadlessPacer: now, sleep until,
 * and the wall clocks it stamps each block with. This one holds a time that
 * moves only when the case moves it, so a case says exactly when the
 * driver wakes and how late: a wake 85 ms behind its deadline on every
 * block is a starved machine, a wake two seconds late is a sleep, and no
 * wake at all is a stall. Nothing here reads a system clock, so the same
 * blocks are rendered on every machine and every run.
 *
 * advance() moves time; settle() waits until the driver has rendered all it
 * owes for that time and is asleep again. Between the two, the engine's
 * clock, its block count and its replies are the case's to assert on.
 */
#pragma once

#include "HeadlessDriver.h"
#include "clock/clock_math.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

class FakePacer : public HeadlessPacer {
public:
    int64_t nowNs() override {
        const std::lock_guard<std::mutex> lk(mMutex);
        return mNow;
    }

    void sleepUntilNs(int64_t deadlineNs, const std::stop_token& stop) override {
        const std::stop_callback wake(stop, [this] { mCv.notify_all(); });
        std::unique_lock<std::mutex> lk(mMutex);
        mDeadline = deadlineNs;
        // Free-running: the deadline is now, and the driver goes straight on.
        // The engine boots this way, one block a sleep, as fast as it renders.
        if (!mHeld) {
            mNow = std::max(mNow, deadlineNs);
            ++mWakes;
            return;
        }
        mSleeping = true;
        mCv.notify_all();   // settle() may be waiting for exactly this
        mCv.wait(lk, [&] { return (!mStalled && mNow >= deadlineNs) || stop.stop_requested(); });
        // A stalled thread is let go only by a stop, and the stall ends with it:
        // the thread started in its place sleeps as a healthy one does.
        if (stop.stop_requested()) mStalled = false;
        mSleeping = false;
        ++mWakes;
    }

    double wallNtp() override {
        const std::lock_guard<std::mutex> lk(mMutex);
        return mOriginNtp + static_cast<double>(mNow) / 1e9;
    }

    double monotonicSeconds() override {
        const std::lock_guard<std::mutex> lk(mMutex);
        return static_cast<double>(mNow) / 1e9;
    }

    // ── The case's side ─────────────────────────────────────────────────────

    // Take the time: from here on it moves only through advance(). Until
    // this, the pacer runs free and the driver sleeps for no time at all,
    // which is how the engine boots and answers its first ping.
    void hold() {
        {
            const std::lock_guard<std::mutex> lk(mMutex);
            mHeld = true;
        }
        mCv.notify_all();
    }

    // The driver's thread is stuck: time may pass, and it does not wake.
    // What the engine sees of a thread spinning inside the OS layer or
    // dead, the wedge the watchdog exists for. Ends when the engine stops
    // that thread; the one it starts in its place wakes as normal.
    void stall() {
        {
            const std::lock_guard<std::mutex> lk(mMutex);
            mStalled = true;
        }
        mCv.notify_all();
    }

    // Time passes. A driver whose deadline this reaches wakes, late by
    // however far past it this went.
    void advance(int64_t ns) {
        {
            const std::lock_guard<std::mutex> lk(mMutex);
            mNow += ns;
        }
        mCv.notify_all();
    }

    void advanceMs(int64_t ms) { advance(ms * 1'000'000); }

    // Until the driver is asleep with a deadline still ahead, or stuck: every
    // block due by now has been rendered. False if it never got there.
    bool settle(int timeoutMs = 5000) {
        std::unique_lock<std::mutex> lk(mMutex);
        return mCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return mSleeping && (mStalled || mDeadline > mNow); });
    }

    // How many times the driver has woken from a sleep.
    uint64_t wakes() {
        const std::lock_guard<std::mutex> lk(mMutex);
        return mWakes;
    }

private:
    std::mutex              mMutex;
    std::condition_variable mCv;
    int64_t  mNow      = 0;
    int64_t  mDeadline = 0;
    bool     mHeld     = false;
    bool     mStalled  = false;
    bool     mSleeping = false;
    uint64_t mWakes    = 0;
    // So the engine's times look like a wall clock's, as every reader expects.
    const double mOriginNtp = wallClockNTP();
};
