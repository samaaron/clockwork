// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_wait_word.cpp — src/workers/wait_word.h: a thread blocks on a word
 * until it changes or a deadline passes, and a wake from any thread (the
 * audio thread included) is two atomic operations when nobody waits.
 *
 *   - a wait returns when another thread bumps the word, and not before;
 *   - a timed wait returns at its deadline when nothing changes;
 *   - a wait on a word that already differs returns at once;
 *   - no wake is lost, however the bumps and the waits interleave;
 *   - the waiter count is back at zero once nobody waits, so a wake makes
 *     no system call;
 *   - a shared word wakes a waiter in another process.
 */
#include <catch2/catch_test_macros.hpp>

#include "workers/wait_word.h"

#include <atomic>
#include <chrono>
#include <thread>

#if !defined(_WIN32)
#  include <sys/mman.h>
#  include <sys/wait.h>
#  include <unistd.h>
#else
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

using namespace std::chrono;
using clockwork::WaitScope;

TEST_CASE("wait word: a wait returns when another thread bumps the word, and not before", "[wait_word]") {
    ClockworkWaitWord w;
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        clockwork::wait(w, 0);
        returned.store(true, std::memory_order_release);
    });
    // The waiter is blocked: it says so in the count, and has not returned.
    while (w.waiters.load(std::memory_order_acquire) == 0) std::this_thread::yield();
    CHECK_FALSE(returned.load(std::memory_order_acquire));
    clockwork::bump(w);
    waiter.join();
    CHECK(returned.load());
    CHECK(w.value.load() == 1);
    CHECK(w.waiters.load() == 0);
}

TEST_CASE("wait word: a timed wait returns at its deadline when nothing changes", "[wait_word]") {
    ClockworkWaitWord w;
    const auto t0 = steady_clock::now();
    const bool changed = clockwork::wait_until(w, 0, t0 + milliseconds(50));
    const auto took = steady_clock::now() - t0;
    CHECK_FALSE(changed);
    CHECK(took >= milliseconds(50));
    CHECK(took < seconds(10));   // only a wait that ignored its deadline gets here
    CHECK(w.waiters.load() == 0);

    // A deadline already past is a look, not a wait.
    CHECK_FALSE(clockwork::wait_until(w, 0, steady_clock::now() - seconds(1)));
}

TEST_CASE("wait word: a wait on a word that already differs returns at once", "[wait_word]") {
    ClockworkWaitWord w;
    clockwork::bump(w);
    CHECK(clockwork::wait_until(w, 0, steady_clock::now() + seconds(10)));
    clockwork::wait(w, 0);   // would block for ever if it waited
    SUCCEED();
}

TEST_CASE("wait word: no wake is lost, however the bumps and the waits interleave", "[wait_word]") {
    // A consumer that only ever sleeps on the word follows a producer that
    // only ever bumps it. A lost wake leaves the consumer asleep with the
    // producer finished, and the deadline below is what that looks like.
    constexpr uint32_t kBumps = 200'000;
    ClockworkWaitWord w;
    std::atomic<uint32_t> produced{0};
    std::atomic<bool> done{false};
    std::thread consumer([&] {
        uint32_t seen = w.value.load(std::memory_order_acquire);
        while (produced.load(std::memory_order_acquire) < kBumps) {
            clockwork::wait_until(w, seen, steady_clock::now() + seconds(30));
            seen = w.value.load(std::memory_order_acquire);
        }
        done.store(true, std::memory_order_release);
    });
    std::thread producer([&] {
        for (uint32_t i = 0; i < kBumps; ++i) {
            produced.fetch_add(1, std::memory_order_release);
            clockwork::bump(w);
        }
    });
    producer.join();
    const auto t0 = steady_clock::now();
    consumer.join();
    INFO("the consumer took " << duration_cast<milliseconds>(steady_clock::now() - t0).count()
         << " ms to finish after the last bump");
    CHECK(done.load());
    CHECK(steady_clock::now() - t0 < seconds(5));
    CHECK(w.waiters.load() == 0);
}

TEST_CASE("wait word: many waiters are all woken by one bump", "[wait_word]") {
    ClockworkWaitWord w;
    std::atomic<int> woke{0};
    std::thread a([&] { clockwork::wait(w, 0); woke.fetch_add(1); });
    std::thread b([&] { clockwork::wait(w, 0); woke.fetch_add(1); });
    std::thread c([&] { clockwork::wait(w, 0); woke.fetch_add(1); });
    while (w.waiters.load(std::memory_order_acquire) < 3) std::this_thread::yield();
    clockwork::bump(w);
    a.join(); b.join(); c.join();
    CHECK(woke.load() == 3);
}

TEST_CASE("wait word: a condition over other state is waited for through the word", "[wait_word]") {
    // A handshake waits for a fact (a counter at zero, a flag set), not for
    // the word: the word only says "look again".
    ClockworkWaitWord w;
    std::atomic<int> inFlight{3};
    std::thread finisher([&] {
        for (int i = 0; i < 3; ++i) {
            inFlight.fetch_sub(1, std::memory_order_release);
            clockwork::bump(w);
        }
    });
    const bool met = clockwork::wait_for_condition(w, [&] { return inFlight.load(std::memory_order_acquire) == 0; },
                                                   steady_clock::now() + seconds(10));
    finisher.join();
    CHECK(met);
    // And a condition that never comes true gives up at the deadline.
    CHECK_FALSE(clockwork::wait_for_condition(w, [] { return false; }, steady_clock::now() + milliseconds(20)));
}

TEST_CASE("wait word: a stoppable sleep lasts to its deadline, and ends when stop is requested", "[wait_word]") {
    std::stop_source running;
    const auto t0 = steady_clock::now();
    CHECK_FALSE(clockwork::sleep_until(running.get_token(), t0 + milliseconds(50)));
    CHECK(steady_clock::now() - t0 >= milliseconds(50));

    // A stop requested before or during the sleep ends it; only a sleep that
    // ignored the stop reaches a minute.
    std::atomic<bool> stopped{false};
    std::jthread sleeper([&](const std::stop_token& stop) {
        stopped.store(clockwork::sleep_until(stop, steady_clock::now() + seconds(60)));
    });
    const auto t1 = steady_clock::now();
    sleeper.request_stop();
    sleeper.join();
    CHECK(stopped.load());
    CHECK(steady_clock::now() - t1 < seconds(30));

    std::stop_source done;
    done.request_stop();
    CHECK(clockwork::sleep_until(done.get_token(), steady_clock::now() + seconds(60)));
}

#if !defined(_WIN32)
TEST_CASE("wait word: a shared word wakes a waiter in another process", "[wait_word][shared]") {
    void* mem = ::mmap(nullptr, sizeof(ClockworkWaitWord), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    REQUIRE(mem != MAP_FAILED);
    auto* w = new (mem) ClockworkWaitWord();
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        const bool changed = clockwork::wait_until(*w, 0, steady_clock::now() + seconds(20), WaitScope::Shared);
        ::_exit(changed ? 0 : 1);
    }
    // The child is asleep on the word: the count it keeps says so.
    const auto deadline = steady_clock::now() + seconds(20);
    while (w->waiters.load(std::memory_order_acquire) == 0 && steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(w->waiters.load() == 1);
    clockwork::bump(*w, WaitScope::Shared);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    w->~ClockworkWaitWord();
    ::munmap(mem, sizeof(ClockworkWaitWord));
}
#else
TEST_CASE("wait word: a shared word on Windows is woken through its event", "[wait_word][shared]") {
    // Windows has no cross-process address wait: a shared word comes with an
    // event, which each process holds a handle to.
    ClockworkWaitWord w;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    REQUIRE(ev != nullptr);
    std::atomic<bool> changed{false};
    std::thread waiter([&] {
        changed = clockwork::wait_until(w, 0, steady_clock::now() + seconds(20), WaitScope::Shared, ev);
    });
    while (w.waiters.load(std::memory_order_acquire) == 0) std::this_thread::yield();
    clockwork::bump(w, WaitScope::Shared, ev);
    waiter.join();
    CHECK(changed.load());
    CloseHandle(ev);
}
#endif
