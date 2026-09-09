// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork
 * Copyright (c) 2025 Sam Aaron
 *
 *
 * RingReader.h — a registry of work items run as one PASS, in registration
 * order: ring drains (buffer + head/tail + callback + optional metric
 * counters) and plain tasks (used for rings whose consumer state lives
 * elsewhere, such as the lanes egress drains in lanes.cpp). The engine's
 * control pass registers the egress drain, the peer command-plane task, the
 * control-ring drain and the MIDI clock producer here, so ONE thread is the
 * sole non-RT consumer of all of them.
 *
 * Whose thread that is, is the caller's choice. start() spawns one that
 * blocks on *wakeWord (a C++20 atomic wait — the audio callback bumps
 * `processCount` every block, so a reader that drains a ring the audio thread
 * fills passes that and drains each block). Or the caller never starts one
 * and calls pass() itself, from a thread of its own, whenever it likes: a host
 * that owns the process runs the engine's control plane on its own thread
 * this way (ClockworkEngine::controlPass). Either way the pass is timed, so
 * the blocking observability below reads the same whoever ran it.
 *
 * JUCE-free — only the destination leaves touch JUCE; the ring transport does
 * not. Metrics: each counter is optional; pass nullptr to skip it.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>
#include "lanes/ring_drain.h"

class RingReader {
public:
    // Optional per-ring counters; any left null is simply not tracked.
    // The drain algorithm itself lives in lanes (ring_drain.h) — this class
    // is only the thread + wake + registration wrapper around it.
    using Metrics = ClockworkDrainMetrics;

    // (sourceId, payload, payloadSize, sequence). sourceId carries the origin
    // token on the control ring; a drain that has no use for it ignores it.
    using OnMessage = std::function<void(uint32_t, const uint8_t*, uint32_t, uint32_t)>;

    explicit RingReader(const char* threadName) : mName(threadName) {
        mDrains.reserve(4);  // fixed before start(); reserve so run() never reallocs
    }
    ~RingReader() { stop(); }

    RingReader(const RingReader&) = delete;
    RingReader& operator=(const RingReader&) = delete;

    // Set the wake word the thread blocks on. Call before start().
    void setWake(std::atomic<uint32_t>* wakeWord) { mWake = wakeWord; }

    // Register a ring to drain on each wake. Call all of these before start()
    // (the drain list is fixed once the thread runs).
    void addDrain(uint8_t*              buffer,
                  uint32_t              bufferSize,
                  std::atomic<int32_t>* head,
                  std::atomic<int32_t>* tail,
                  OnMessage             onMessage,
                  Metrics               metrics);

    // Register a task to run on each wake, in registration order with the ring
    // drains. Used for rings whose consumer state lives elsewhere — the lanes
    // egress drains (clockwork_egress_rt_drain / clockwork_egress_nrt_drain) own theirs.
    void addTask(std::function<void()> task);

    // Spawn the thread (after setWake + all addDrain/addTask). Idempotent.
    void start();
    // Signal exit, wake the loop (and release any park), join. Idempotent.
    void stop();
    // Whether start() spawned a thread that has not been stopped.
    bool running() const { return mThread.joinable(); }

    // ONE pass, on the calling thread: every registered drain and task, in
    // order, timed like the thread's own passes (maxPassUs & co. below record
    // it). For a caller that owns the pass instead of starting the thread —
    // and only then: the drains are single-consumer, so a pass on a second
    // thread beside a running one is what the rings forbid. The registry is
    // fixed once the first pass runs, as it is once the thread does.
    void pass();

    // Quiescent pause: returns once the run loop is parked between drain
    // passes (or the thread has exited), so the caller may safely reset the
    // ring/drain state the drains read — cold swap tears down and re-inits the
    // shared-memory arena while this thread would otherwise keep draining.
    // No-op if the thread was never started or when called from this reader's
    // own thread (a drain handler that triggers the swap is by definition not
    // draining concurrently with it). resume() unparks; extra resumes are
    // harmless.
    void pause();
    void resume();

    // ── Blocking observability ───────────────────────────────────────────────
    // This thread is the sole non-RT consumer: a handler that blocks stops every
    // later control command AND the egress drains registered behind it, so the
    // server keeps accepting packets it can no longer answer. Nothing about that
    // is visible from outside — socket up, process alive, audio ticking — which
    // is how one stall cost a user a 30 s boot and told them only that the
    // client would not connect.

    // Longest drain pass observed, microseconds (high-water mark).
    uint32_t maxPassUs() const { return mMaxPassUs.load(std::memory_order_relaxed); }
    void resetMaxPassUs();

    // Longest drain pass completed within the trailing window (~60 s),
    // microseconds. A since-boot high-water mark can only ever grow, so it
    // says nothing about whether the stall was five seconds or five hours
    // ago — this decays back to quiet once the window slides past.
    uint32_t recentMaxPassUs() const;

    // Microseconds the current pass has been running; 0 between passes. A
    // high-water mark only records a stall once it ENDS — this shows one that
    // is still happening, which is the state a wedged server is stuck in.
    uint32_t inFlightUs() const;

    // Invoked (on the thread that ran the pass) when a pass exceeds the
    // threshold, with its duration. The engine hooks this to name the command
    // that was in flight.
    void onSlowPass(std::function<void(uint32_t)> fn) { mOnSlowPass = std::move(fn); }
    void setSlowPassThresholdUs(uint32_t us) { mSlowPassThresholdUs = us; }

private:
    void run();
    static uint64_t nowUs();

    struct Drain {
        uint8_t*              buffer = nullptr;
        uint32_t              size   = 0;
        std::atomic<int32_t>* head   = nullptr;
        std::atomic<int32_t>* tail   = nullptr;
        OnMessage             onMessage;
        Metrics               metrics;
        ClockworkDrainState          state;
        std::function<void()> task;  // when set, run instead of the ring walk
    };
    void drainOne(Drain& d);

    const char*            mName;
    std::atomic<uint32_t>* mWake     = nullptr;
    uint32_t               mLastWake = 0;
    std::vector<Drain>     mDrains;
    std::atomic<bool>      mExit{false};

    // pause()/resume() handshake. mPauseRequest is the caller's ask; mParked
    // acknowledges that the run loop is outside any drain pass.
    std::atomic<uint32_t>  mPauseRequest{0};
    std::atomic<uint32_t>  mParked{0};

    // Pass timing. mPassStartUs is a steady-clock stamp while a pass runs and 0
    // between passes, so a reader can tell "blocked now" from "was slow once".
    std::atomic<uint64_t>  mPassStartUs{0};
    std::atomic<uint32_t>  mMaxPassUs{0};
    // Rolling-window worst: per-bucket max, keyed by the bucket's absolute
    // epoch (nowUs / kRecentBucketUs) so a reader can drop buckets that have
    // slid out of the window without any timer. Written by the reader thread
    // and by resetMaxPassUs(); relaxed loads elsewhere (display-only).
    static constexpr uint32_t kRecentBuckets  = 12;
    static constexpr uint64_t kRecentBucketUs = 5'000'000;   // 12 x 5 s = 60 s window
    std::atomic<uint64_t>  mRecentEpoch[kRecentBuckets] {};
    std::atomic<uint32_t>  mRecentMaxUs[kRecentBuckets] {};
    uint32_t               mSlowPassThresholdUs = 250'000;   // 250 ms
    std::function<void(uint32_t)> mOnSlowPass;

    std::thread            mThread;
};
