// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * RingReader.cpp — see RingReader.h.
 */
#include "RingReader.h"
#include "clockwork_config.h"   // clockwork_log

// The reader whose thread this is, set by run() before its first pass, so
// running() and pause() can answer a call from its own handler without
// reading mThread — which start() may still be assigning while a first
// pass's handler is already running.
static thread_local const RingReader* tReaderOfThisThread = nullptr;

void RingReader::start() {
    if (mThread.joinable()) return;
    const uint32_t seen = mWake->value.load(std::memory_order_seq_cst);
    mThread = std::jthread([this, seen](const std::stop_token& stop) { run(stop, seen); });
}

bool RingReader::running() const {
    return tReaderOfThisThread == this || mThread.joinable();
}

// request_stop() runs wakeForStop through the loop's stop_callback, so the
// thread wakes and leaves; the destructor does the same without stop().
void RingReader::stop() {
    if (!mThread.joinable()) return;
    mThread.request_stop();
    mThread.join();
}

void RingReader::wakeForStop() {
    wake();    // out of the wait for work
    resume();  // out of a park
}

bool RingReader::pause() {
    // Self-pause would deadlock waiting for our own park acknowledgement. It is
    // also unnecessary: a drain handler that triggers the caller's critical
    // section (e.g. a cold swap run from an OSC command on the NRT gateway
    // thread) is by definition not draining concurrently with it.
    if (tReaderOfThisThread == this) return true;
    if (!mThread.joinable()) return true;
    // An odd count is "pause asked"; this ask is the count it makes.
    uint32_t ask = mPauseRequests.value.load(std::memory_order_seq_cst);
    if ((ask & 1u) == 0) {
        ask = mPauseRequests.value.fetch_add(1, std::memory_order_seq_cst) + 1;
        clockwork::notify(mPauseRequests);
    }
    wake();   // a reader waiting for work comes round to the park
    // The acknowledgement normally lands within one drain pass. The deadline
    // stays: a reader wedged behind a foreign lock must degrade to an unparked
    // (pre-pause) swap, not hang the device switch.
    const bool parked = clockwork::wait_for_condition(
        mParkedAt, [&] { return mParkedAt.value.load(std::memory_order_acquire) == ask; },
        clockwork::WaitClock::now() + std::chrono::seconds(2));
    if (!parked)
        clockwork_log("[%s] pause: reader did not park within 2s — proceeding unparked", mName);
    return parked;
}

void RingReader::resume() {
    uint32_t ask = mPauseRequests.value.load(std::memory_order_seq_cst);
    while ((ask & 1u) != 0
           && !mPauseRequests.value.compare_exchange_weak(ask, ask + 1, std::memory_order_seq_cst)) {}
    clockwork::notify(mPauseRequests);
}

void RingReader::addDrain(uint8_t*              buffer,
                          uint32_t              bufferSize,
                          std::atomic<int32_t>* head,
                          std::atomic<int32_t>* tail,
                          OnMessage             onMessage,
                          Metrics               metrics) {
    Drain d;
    d.buffer    = buffer;
    d.size      = bufferSize;
    d.head      = head;
    d.tail      = tail;
    d.onMessage = std::move(onMessage);
    d.metrics   = metrics;
    mDrains.push_back(std::move(d));
}

void RingReader::addDrain(uint8_t* buffer, uint32_t bufferSize,
                          std::atomic<int32_t>* head, std::atomic<int32_t>* tail,
                          OnMessageVerdict onMessage, Metrics metrics) {
    Drain d;
    d.buffer           = buffer;
    d.size             = bufferSize;
    d.head             = head;
    d.tail             = tail;
    d.onMessageVerdict = std::move(onMessage);
    d.metrics          = metrics;
    mDrains.push_back(std::move(d));
}

void RingReader::addTask(std::function<void()> task) {
    Drain d;
    d.task = std::move(task);
    mDrains.push_back(std::move(d));
}

void RingReader::drainOne(Drain& d) {
    if (d.task) {
        d.task();
        return;
    }
    // The walk itself is the shared lanes algorithm (src/lanes/ring_drain.h)
    // — the same code the lanes egress drains run. This class only adds the
    // thread, the wake word and the drain registry.
    clockwork_drain_ring(d.buffer, d.size, d.head, d.tail, d.state, d.metrics,
                  0 /* drain everything available */,
                  [&d](uint32_t sourceId, const uint8_t* payload,
                       uint32_t payloadSize, uint32_t sequence) {
                      if (d.onMessageVerdict)
                          return d.onMessageVerdict(sourceId, payload, payloadSize, sequence);
                      if (d.onMessage)
                          d.onMessage(sourceId, payload, payloadSize, sequence);
                      return ClockworkDrainVerdict::Consume;
                  });
}

void RingReader::run(const std::stop_token& stop, uint32_t seen) {
    tReaderOfThisThread = this;
    const std::stop_callback wakeOnStop(stop, [this] { wakeForStop(); });

    while (!stop.stop_requested()) {
        // Asleep until the word changes: an audio block, a write to a ring
        // this drains (in this process or a peer's), a pause, a stop.
        clockwork::wait(*mWake, seen, mWakeScope, mWakeEvent);
        // Read before the pass: a wake that lands while the pass runs is a
        // change the next wait sees at once, and another pass.
        seen = mWake->value.load(std::memory_order_seq_cst);
        if (stop.stop_requested()) break;

        const uint32_t ask = mPauseRequests.value.load(std::memory_order_seq_cst);
        if ((ask & 1u) != 0) {
            // Acknowledge between drain passes — the release makes every write
            // of the pass visible to the pauser — then sleep until resume()
            // (or a stop, which resumes) moves the count on.
            mParkedAt.value.store(ask, std::memory_order_release);
            clockwork::notify(mParkedAt);
            clockwork::wait(mPauseRequests, ask);
            continue;
        }

        pass();
    }
}

void RingReader::pass() {
    // Time the whole pass rather than each drain: what matters is how long
    // the thread running it was unavailable to everything queued behind it.
    const uint64_t startUs = nowUs();
    mPassStartUs.store(startUs, std::memory_order_relaxed);

    for (auto& d : mDrains) drainOne(d);
    mPasses.fetch_add(1, std::memory_order_relaxed);

    const uint64_t elapsed = nowUs() - startUs;
    mPassStartUs.store(0, std::memory_order_relaxed);
    const uint32_t us = static_cast<uint32_t>(
        elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
    if (us > mMaxPassUs.load(std::memory_order_relaxed))
        mMaxPassUs.store(us, std::memory_order_relaxed);
    // Rolling-window worst: bucket by the pass END time. Reset a slot the
    // first time its epoch comes round again (the wrap means its old value
    // is a full window old).
    const uint64_t epoch = (startUs + elapsed) / kRecentBucketUs;
    const uint32_t slot  = static_cast<uint32_t>(epoch % kRecentBuckets);
    if (mRecentEpoch[slot].load(std::memory_order_relaxed) != epoch) {
        mRecentMaxUs[slot].store(0, std::memory_order_relaxed);
        mRecentEpoch[slot].store(epoch, std::memory_order_relaxed);
    }
    if (us > mRecentMaxUs[slot].load(std::memory_order_relaxed))
        mRecentMaxUs[slot].store(us, std::memory_order_relaxed);
    if (us >= mSlowPassThresholdUs && mOnSlowPass) mOnSlowPass(us);
}

uint64_t RingReader::nowUs() const {
    if (mClock) return mClock();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t RingReader::recentMaxPassUs() const {
    const uint64_t nowEpoch = nowUs() / kRecentBucketUs;
    uint32_t worst = 0;
    for (uint32_t i = 0; i < kRecentBuckets; ++i) {
        // A slot is live only while its epoch is within the last kRecentBuckets
        // ticks; older slots are leftovers from a previous window lap.
        if (mRecentEpoch[i].load(std::memory_order_relaxed) + kRecentBuckets <= nowEpoch)
            continue;
        const uint32_t us = mRecentMaxUs[i].load(std::memory_order_relaxed);
        if (us > worst) worst = us;
    }
    return worst;
}

void RingReader::resetMaxPassUs() {
    mMaxPassUs.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kRecentBuckets; ++i) {
        mRecentEpoch[i].store(0, std::memory_order_relaxed);
        mRecentMaxUs[i].store(0, std::memory_order_relaxed);
    }
}

uint32_t RingReader::inFlightUs() const {
    const uint64_t start = mPassStartUs.load(std::memory_order_relaxed);
    if (start == 0) return 0;
    const uint64_t elapsed = nowUs() - start;
    return static_cast<uint32_t>(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
}
