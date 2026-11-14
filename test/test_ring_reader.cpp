// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_ring_reader.cpp — clockwork's non-realtime ring reader thread.
 *
 * Contract pinned here: RingReader runs a thread that blocks on its wake word
 * (src/workers/wait_word.h) and runs one pass of its drains each time the
 * word changes: the audio thread wakes it every block, a writer of a ring it
 * drains wakes it after the write, a peer in another process wakes it
 * through a shared word. It never wakes on a timer. It is pure transport — it
 * never looks inside a frame — so everything below uses arbitrary byte
 * payloads and holds whichever DSP is attached.
 *
 *   1. Stopping the reader does not depend on anyone else waking it.
 *
 *   2. A frame written to a drained ring and followed by one wake is
 *      delivered to the onMessage callback, payload and writer label intact.
 *      An idle reader runs no pass at all, and no wake is lost however the
 *      writes, the wakes and the passes interleave.
 *
 *   3. pause() is quiescent and safe from any thread: once it returns, no
 *      drain runs until resume() (a device swap resets the ring/drain state
 *      the drains walk), and calling it from the reader's own thread (a
 *      handler inside a drain triggering the swap) is a no-op rather than a
 *      self-deadlock.
 *
 *   4. Blocking observability: this thread is the sole non-RT consumer, so a
 *      handler that blocks stops everything registered behind it while the
 *      process still looks alive. The reader must be able to say how long a
 *      pass took, and say it WHILE it is still stuck.
 */
#include <catch2/catch_test_macros.hpp>

#include "RingReader.h"
#include "RingBufferWriter.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

// A clock the case moves (RingReader::setClock): a pass takes exactly the
// time the case gives it, however the machine schedules the reader, and an
// idle pass takes none. It starts far from 0, which the reader keeps for "no
// pass in flight".
struct CaseClock {
    std::atomic<uint64_t> us{1'000'000'000};
    void advance(uint64_t by) { us.fetch_add(by, std::memory_order_acq_rel); }
    std::function<uint64_t()> reading() {
        return [this] { return us.load(std::memory_order_acquire); };
    }
};

}  // namespace

TEST_CASE("RingReader stops promptly when its wake word is never bumped",
          "[RingReader]") {

    auto reader = std::make_unique<RingReader>("test-ringreader-stop");
    reader->start();

    // Give the run loop time to reach its blocking wait on `wake`.
    std::this_thread::sleep_for(milliseconds(50));

    // Nobody ever changes `wake`. Destroying the reader must still stop the
    // thread quickly. The broken path (notify_all without a value change)
    // leaves the thread parked in atomic::wait, so the destructor blocks for
    // the join timeout.
    const auto t0 = steady_clock::now();
    reader.reset();
    const auto elapsedMs =
        duration_cast<milliseconds>(steady_clock::now() - t0).count();

    // The broken path never returns (the join has no timeout), so any bound
    // tells the two apart; this one only a wedge reaches.
    INFO("RingReader destructor took " << elapsedMs << " ms");
    REQUIRE(elapsedMs < 30000);
}

TEST_CASE("RingReader delivers a message after its wake word is bumped",
          "[RingReader]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};

    std::mutex rxMutex;
    std::string received;
    std::atomic<uint32_t> gotSource{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-drain");
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t sourceId, const uint8_t* p, uint32_t n, uint32_t) {
            std::lock_guard<std::mutex> lock(rxMutex);
            received.assign(reinterpret_cast<const char*>(p), n);
            gotSource.store(sourceId, std::memory_order_release);
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    // Arbitrary bytes: the reader never parses a payload, so this only has to
    // be something recognisable on the far side.
    const std::string payload = "hello-reader";
    const uint32_t kSourceId = 7;
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), kSourceId));

    // One wake, as a writer gives it: the reader runs a pass and delivers.
    reader.wake();
    const auto deadline = steady_clock::now() + seconds(10);
    while (gotCount.load(std::memory_order_acquire) == 0 && steady_clock::now() < deadline)
        std::this_thread::yield();

    REQUIRE(gotCount.load(std::memory_order_acquire) == 1);
    REQUIRE(gotSource.load(std::memory_order_acquire) == kSourceId);
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        REQUIRE(received == payload);
    }
}

TEST_CASE("RingReader pause parks draining until resume", "[RingReader]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-pause");

    // pause() before start() is a no-op, not a hang.
    reader.pause();
    reader.resume();

    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    reader.pause();

    // While parked, a written message plus continuous wake ticks must not
    // reach the callback.
    const std::string payload = "parked";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));
    for (int i = 0; i < 50; i++) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) == 0);

    // After resume the pending message is drained.
    reader.resume();
    const auto deadline = steady_clock::now() + seconds(2);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(2));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) == 1);
}

TEST_CASE("RingReader pause from its own thread is a no-op, not a deadlock",
          "[RingReader]") {
    std::atomic<bool> selfPaused{false};
    std::atomic<bool> sawRunning{false};
    std::atomic<int> passes{0};

    // The wakes come from a thread started before start(), so nothing this
    // thread does after start() is ordered before the handler — only the
    // reader thread's creation is. A wake bumped here straight after start()
    // would order it on a quick machine and hide a pause() that reads mThread
    // while start() is still assigning it.
    RingReader reader("test-ringreader-selfpause");
    // Mirrors a drain handler that triggers a device swap: the swap parks the
    // readers, including the one whose handler is running — asking whether the
    // reader runs and then pausing it, as ClockworkEngine::parkControlPass does.
    reader.addTask([&]() {
        if (!selfPaused.exchange(true)) {
            sawRunning.store(reader.running());
            reader.pause();
        }
        passes.fetch_add(1, std::memory_order_release);
    });
    std::jthread waker([&](const std::stop_token& st) {
        while (!st.stop_requested()) {
            reader.wake();
            std::this_thread::yield();
        }
    });
    reader.start();

    // If self-pause deadlocked, the task would never complete a second pass.
    const auto deadline = steady_clock::now() + seconds(2);
    while (passes.load(std::memory_order_acquire) < 2
           && steady_clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(2));
    waker.request_stop();
    waker.join();
    REQUIRE(selfPaused.load());
    CHECK(sawRunning.load());
    REQUIRE(passes.load(std::memory_order_acquire) >= 2);
}

// ── Blocking observability ───────────────────────────────────────────────────
//
// One thread drains control commands AND the egress behind them, so a handler
// that blocks stops clockwork answering anything while its socket stays up
// and its audio keeps ticking, with no log line or metric of its own.
//
// These pin the measurement rather than any particular slow caller: whatever
// blocks, the reader must be able to say how long for, and say it WHILE stuck
// (a high-water mark alone only speaks once the stall is over).

TEST_CASE("RingReader runs a pass on the caller's thread when no thread was started",
          "[RingReader][pass]") {
    // A host that owns the pass never calls start(): it calls pass() from a
    // thread of its own. The registry, the drain and the timing are the
    // thread's own, so a pass run this way must drain what the thread's
    // would and be measured as the thread's would.
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};

    std::string received;
    uint32_t gotSource = 0;
    int gotCount = 0;
    int tasks = 0;

    RingReader reader("test-ringreader-pass");
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t sourceId, const uint8_t* p, uint32_t n, uint32_t) {
            received.assign(reinterpret_cast<const char*>(p), n);
            gotSource = sourceId;
            ++gotCount;
            std::this_thread::sleep_for(milliseconds(30));
        },
        RingReader::Metrics{});
    reader.addTask([&] { ++tasks; });

    REQUIRE_FALSE(reader.running());

    // An empty pass drains nothing, runs the task, and is timed.
    reader.pass();
    REQUIRE(gotCount == 0);
    REQUIRE(tasks == 1);
    REQUIRE(reader.inFlightUs() == 0);

    const std::string payload = "by-hand";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 11));

    reader.pass();
    REQUIRE(gotCount == 1);
    REQUIRE(gotSource == 11);
    REQUIRE(received == payload);
    REQUIRE(tasks == 2);
    // The blocking handler was measured, as it would be on the thread.
    REQUIRE(reader.maxPassUs() >= 25'000);
    REQUIRE(reader.recentMaxPassUs() >= 25'000);
    REQUIRE(reader.inFlightUs() == 0);

    // Nothing left: a further pass takes nothing and still runs the task.
    reader.pass();
    REQUIRE(gotCount == 1);
    REQUIRE(tasks == 3);
    REQUIRE_FALSE(reader.running());
    // stop() on a reader that never started is a no-op, not a hang.
    reader.stop();
}

TEST_CASE("RingReader reports how long a blocking handler held the thread",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-slow");
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            std::this_thread::sleep_for(milliseconds(120));
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});

    // Nothing has run, so nothing has blocked. Read before the thread starts:
    // once it runs, its idle passes are timed too, and on the real clock of a
    // slow machine they take a microsecond or two. (The cases below hold idle
    // passes to exactly nothing, on a clock of their own.)
    REQUIRE(reader.maxPassUs() == 0);
    reader.start();

    const std::string payload = "slow";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    const auto deadline = steady_clock::now() + seconds(5);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) >= 1);

    // The handler counts itself in from INSIDE the pass, and the pass records
    // its time only after every drain has returned: read now and the record
    // may not be there yet. Joining the reader puts the finished pass, and
    // all it recorded, before the reads — an order, not a wait.
    reader.stop();

    // The reader measures itself, so this holds however starved the test
    // process was — unlike a wall-clock deadline in the test thread.
    REQUIRE(reader.maxPassUs() >= 100'000);

    reader.resetMaxPassUs();
    REQUIRE(reader.maxPassUs() == 0);
}

TEST_CASE("RingReader tracks the worst pass in the recent window",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<int> gotCount{0};

    CaseClock clock;

    RingReader reader("test-ringreader-recent");
    // On the case's clock: the handler holds the thread for exactly 120 ms of
    // it, and the passes around it take nothing.
    reader.setClock(clock.reading());
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            clock.advance(120'000);
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    REQUIRE(reader.recentMaxPassUs() == 0);

    const std::string payload = "recent";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    const auto deadline = steady_clock::now() + seconds(5);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) >= 1);
    reader.stop();   // the pass's record is written after its handler returns

    // The slow pass just ended, so it is inside the rolling window and the
    // two measures agree; only the windowed copy lets it go once the window
    // slides past (the next case).
    REQUIRE(reader.recentMaxPassUs() == 120'000);
    REQUIRE(reader.maxPassUs() == 120'000);

    // Reset clears the window along with the high-water mark.
    reader.resetMaxPassUs();
    REQUIRE(reader.recentMaxPassUs() == 0);
}

TEST_CASE("RingReader lets a slow pass go from the recent window once the window has slid past it",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<int> gotCount{0};
    CaseClock clock;

    RingReader reader("test-ringreader-window");
    reader.setClock(clock.reading());
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            clock.advance(120'000);
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    const std::string payload = "window";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    const auto deadline = steady_clock::now() + seconds(5);
    while (gotCount.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(gotCount.load(std::memory_order_acquire) >= 1);
    reader.stop();   // the pass's record is written after its handler returns
    REQUIRE(reader.recentMaxPassUs() == 120'000);

    // The window is 12 buckets of 5 s (RingReader.h), and a pass belongs to
    // the bucket it ended in: 55 s on, it is still in the window, wherever in
    // its bucket it ended; 60 s on, it has slid out, and only the high-water
    // mark keeps it.
    clock.advance(55'000'000);
    REQUIRE(reader.recentMaxPassUs() == 120'000);
    clock.advance(5'000'000);
    REQUIRE(reader.recentMaxPassUs() == 0);
    REQUIRE(reader.maxPassUs() == 120'000);
}

TEST_CASE("RingReader exposes a stall while it is still stuck",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<bool> release{false};
    std::atomic<bool> inHandler{false};

    CaseClock clock;

    RingReader reader("test-ringreader-inflight");
    // However the case leaves, the handler is let go before the reader is
    // joined: a check that fails is a failure, not a hang.
    struct LetGo {
        std::atomic<bool>& release;
        ~LetGo() { release.store(true, std::memory_order_release); }
    } letGo{release};
    // On the case's clock: the stall lasts exactly as long as the case moves
    // the clock on while the handler is held, and idle passes take nothing.
    reader.setClock(clock.reading());
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            inHandler.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
                std::this_thread::sleep_for(milliseconds(2));
        },
        RingReader::Metrics{});
    reader.start();

    // Idle: not blocked.
    REQUIRE(reader.inFlightUs() == 0);

    const std::string payload = "wedge";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    auto deadline = steady_clock::now() + seconds(5);
    while (!inHandler.load(std::memory_order_acquire)
           && steady_clock::now() < deadline) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(5));
    }
    REQUIRE(inHandler.load(std::memory_order_acquire));

    // Wedged right now: this is what a watchdog or the metrics panel reads to
    // distinguish "busy" from "can never answer you".
    clock.advance(60'000);
    REQUIRE(reader.inFlightUs() == 60'000);
    REQUIRE(reader.maxPassUs() == 0);   // the pass has not ended yet

    release.store(true, std::memory_order_release);

    // The pass clears its in-flight mark before it records its time, so an
    // in-flight of 0 does not yet mean the record is there. Joining the
    // reader does: the released pass has finished, and all of it is visible.
    reader.stop();
    REQUIRE(reader.inFlightUs() == 0);        // released
    REQUIRE(reader.maxPassUs() == 60'000);    // and recorded on the way out
}

TEST_CASE("RingReader reports a slow pass to its observer",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<uint32_t> reportedUs{0};
    std::atomic<int> reports{0};

    RingReader reader("test-ringreader-report");
    reader.setSlowPassThresholdUs(50'000);
    reader.onSlowPass([&](uint32_t us) {
        reportedUs.store(us, std::memory_order_release);
        reports.fetch_add(1, std::memory_order_release);
    });
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            std::this_thread::sleep_for(milliseconds(80));
        },
        RingReader::Metrics{});
    reader.start();

    const std::string payload = "report";
    REQUIRE(RingBufferWriter::write(
        buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
        payload.data(), static_cast<uint32_t>(payload.size()), 0));

    const auto deadline = steady_clock::now() + seconds(5);
    while (reports.load(std::memory_order_acquire) == 0
           && steady_clock::now() < deadline) {
        reader.wake();
        std::this_thread::sleep_for(milliseconds(5));
    }

    REQUIRE(reports.load(std::memory_order_acquire) >= 1);
    REQUIRE(reportedUs.load(std::memory_order_acquire) >= 50'000);
}

TEST_CASE("RingReader stays quiet when passes are quick",
          "[RingReader][blocking]") {
    constexpr uint32_t kSize = 4096;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<int> reports{0};
    std::atomic<int> gotCount{0};

    RingReader reader("test-ringreader-quiet");
    reader.setSlowPassThresholdUs(250'000);
    // Passes are timed on the case's clock, which stands still: a pass the
    // machine preempts is still a quick pass, as it is in the reader's terms.
    reader.setClock([] { return uint64_t{0}; });
    reader.onSlowPass([&](uint32_t) { reports.fetch_add(1, std::memory_order_release); });
    reader.addDrain(
        buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
            gotCount.fetch_add(1, std::memory_order_release);
        },
        RingReader::Metrics{});
    reader.start();

    for (int i = 0; i < 20; i++) {
        const std::string payload = "quick";
        REQUIRE(RingBufferWriter::write(
            buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
            payload.data(), static_cast<uint32_t>(payload.size()), 0));
        reader.wake();
        std::this_thread::sleep_for(milliseconds(2));
    }

    REQUIRE(reports.load(std::memory_order_acquire) == 0);
}

// The reader runs a pass when something gives it work, and otherwise sleeps:
// no timer, so an idle engine costs no wakeups.
TEST_CASE("RingReader: an idle reader runs no pass", "[RingReader][wake]") {
    RingReader reader("test-ringreader-idle");
    std::atomic<int> passes{0};
    reader.addTask([&] { passes.fetch_add(1, std::memory_order_relaxed); });
    reader.start();
    std::this_thread::sleep_for(milliseconds(100));   // what a 1 ms timer would fill with a hundred passes
    CHECK(passes.load() == 0);
    CHECK(reader.passes() == 0);

    reader.wake();
    const auto deadline = steady_clock::now() + seconds(10);
    while (reader.passes() == 0 && steady_clock::now() < deadline) std::this_thread::yield();
    CHECK(reader.passes() == 1);
    std::this_thread::sleep_for(milliseconds(50));
    CHECK(reader.passes() == 1);   // one wake, one pass
}

// A writer that wakes the reader after every write never has a frame left in
// the ring, however its writes and the reader's passes interleave: a wake that
// lands while a pass runs makes the reader pass again.
TEST_CASE("RingReader: no wake is lost, however writes and passes interleave", "[RingReader][wake]") {
    constexpr uint32_t kSize = 64 * 1024;
    constexpr int kFrames = 50'000;
    std::vector<uint8_t> buffer(kSize, 0);
    std::atomic<int32_t> head{0}, tail{0}, sequence{0}, writeLock{0};
    std::atomic<int> delivered{0};

    RingReader reader("test-ringreader-nolost");
    reader.addDrain(buffer.data(), kSize, &head, &tail,
        [&](uint32_t, const uint8_t*, uint32_t, uint32_t) { delivered.fetch_add(1, std::memory_order_release); },
        RingReader::Metrics{});
    reader.start();

    const uint32_t frame = 0xC10C;
    for (int i = 0; i < kFrames; ++i) {
        // A full ring is backpressure: the writer waits for the reader to
        // drain, as a client retries a full ingress ring.
        while (!RingBufferWriter::write(buffer.data(), kSize, &head, &tail, &sequence, &writeLock,
                                        &frame, sizeof frame, 1))
            std::this_thread::yield();
        reader.wake();
    }
    const auto deadline = steady_clock::now() + seconds(10);
    while (delivered.load(std::memory_order_acquire) < kFrames && steady_clock::now() < deadline)
        std::this_thread::yield();
    INFO("delivered " << delivered.load() << " of " << kFrames << " in " << reader.passes() << " passes");
    CHECK(delivered.load() == kFrames);
}

// The audio thread wakes the reader every block: with the reader asleep that
// is one wake of the operating system; with the reader busy it is none, only
// the word.
TEST_CASE("RingReader: a wake while the reader is busy costs no system call", "[RingReader][wake]") {
    RingReader reader("test-ringreader-busy");
    std::atomic<bool> inPass{false}, release{false};
    reader.addTask([&] {
        inPass.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    reader.start();
    reader.wake();
    const auto entered = steady_clock::now() + seconds(10);
    while (!inPass.load(std::memory_order_acquire) && steady_clock::now() < entered)
        std::this_thread::yield();
    REQUIRE(inPass.load(std::memory_order_acquire));
    // Busy in a pass: nobody is waiting on the word, so a wake is the word only.
    CHECK(reader.wakeWord()->waiters.load() == 0);
    reader.wake();
    release.store(true, std::memory_order_release);
    // The wake that landed mid-pass is a second pass, not a lost one.
    const auto deadline = steady_clock::now() + seconds(10);
    while (reader.passes() < 2 && steady_clock::now() < deadline) std::this_thread::yield();
    CHECK(reader.passes() == 2);
}
