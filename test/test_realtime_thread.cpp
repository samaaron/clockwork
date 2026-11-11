// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_realtime_thread.cpp — the audio-thread realtime-promotion helper
 * (elevateCurrentThreadToRealtime, src/native/RealtimeThread.*).
 *
 * The tests pin two properties: when rtprio is permitted the calling thread
 * becomes SCHED_RR, and when it is denied the thread is left exactly as it was
 * (no abort, no change).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <pthread.h>
#include <sched.h>   // SCHED_*
#endif

#include "native/RealtimeThread.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>

using clockwork::elevateCurrentThreadToRealtime;
using clockwork::RealtimeStatus;

TEST_CASE("elevateCurrentThreadToRealtime: graceful fallback or genuine RT", "[realtime]") {
    // Run on a dedicated thread: the helper permanently changes the calling
    // thread's scheduling, so promoting the shared Catch2 runner would leak
    // SCHED_RR into later tests. Capture as plain values, assert on this thread.
    RealtimeStatus status{};
    int beforePolicy = 0, afterPolicy = 0, beforePrio = 0, afterPrio = 0;
    bool readOk = false;

    std::thread([&] {
        sched_param before{}, after{};
        const bool gotBefore = pthread_getschedparam(pthread_self(), &beforePolicy, &before) == 0;
        beforePrio = before.sched_priority;

        status = elevateCurrentThreadToRealtime().status;

        const bool gotAfter = pthread_getschedparam(pthread_self(), &afterPolicy, &after) == 0;
        afterPrio = after.sched_priority;
        readOk = gotBefore && gotAfter;
    }).join();

    REQUIRE(readOk);
    if (status == RealtimeStatus::Applied) {
        // Permitted: the thread is now realtime.
        REQUIRE(afterPolicy == SCHED_RR);
        REQUIRE(afterPrio >= sched_get_priority_min(SCHED_RR));
        REQUIRE(afterPrio <= sched_get_priority_max(SCHED_RR));
    } else {
        // Denied: the thread must be left untouched.
        REQUIRE((status == RealtimeStatus::NotPermitted || status == RealtimeStatus::Failed));
        REQUIRE(afterPolicy == beforePolicy);
        REQUIRE(afterPrio == beforePrio);
    }
}

TEST_CASE("elevateCurrentThreadToRealtime: idempotent and never throws", "[realtime]") {
    // Calling twice (as a cold-swap re-opening the device would) must be safe
    // and converge to the same status. On a dedicated thread for the same
    // runner-isolation reason as above.
    RealtimeStatus first{}, second{};
    std::thread([&] {
        first  = elevateCurrentThreadToRealtime().status;
        second = elevateCurrentThreadToRealtime().status;
    }).join();
    REQUIRE(first == second);
}

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

TEST_CASE("elevateCurrentThreadToRealtime: Windows gets MMCSS + "
          "time-critical, never a silent no-op", "[realtime]") {
    // Windows must actively protect the audio thread: JUCE's DirectSound
    // backend polls from an ordinary-priority thread with no MMCSS
    // registration, so without elevation any CPU load preempts the render
    // into audible overruns. On a dedicated thread (the promotion is
    // permanent) with plain-value capture; assertions on the runner thread.
    clockwork::RealtimeResult result{};
    int win32Priority = 0;
    std::thread([&] {
        result        = clockwork::elevateCurrentThreadToRealtime();
        win32Priority = GetThreadPriority(GetCurrentThread());
    }).join();

    REQUIRE(result.status == clockwork::RealtimeStatus::Applied);
    REQUIRE(win32Priority == THREAD_PRIORITY_TIME_CRITICAL);
    // policy carries MMCSS engagement (1 = "Pro Audio" registered). Stripped
    // environments can lack the MMCSS service, and TIME_CRITICAL alone still
    // counts as Applied — assert the field is well-formed, not its value.
    REQUIRE((result.policy == 0 || result.policy == 1));
    REQUIRE(result.error == 0);
}

TEST_CASE("elevateCurrentThreadToRealtime: idempotent on Windows", "[realtime]") {
    clockwork::RealtimeStatus first{}, second{};
    std::thread([&] {
        first  = clockwork::elevateCurrentThreadToRealtime().status;
        second = clockwork::elevateCurrentThreadToRealtime().status;
    }).join();
    REQUIRE(first == clockwork::RealtimeStatus::Applied);
    REQUIRE(first == second);
}

#else  // other platforms (macOS): the helper stays a documented no-op.

TEST_CASE("elevateCurrentThreadToRealtime is a no-op on this platform", "[realtime]") {
    REQUIRE(clockwork::elevateCurrentThreadToRealtime().status
            == clockwork::RealtimeStatus::NotSupported);
}

#endif

// How the calling thread is scheduled, as the OS has it: the audio thread's
// line as each device starts ([device-setup] audio thread …). Read on threads
// of the case's own, whose scheduling it set.
TEST_CASE("currentThreadScheduling says how the calling thread is scheduled", "[realtime]") {
    using Kind = clockwork::ThreadScheduling::Kind;
    clockwork::ThreadScheduling plain{};
    std::thread([&] { plain = clockwork::currentThreadScheduling(); }).join();
#if defined(_WIN32)
    CHECK(plain.kind == Kind::Windows);
    CHECK(plain.priority == THREAD_PRIORITY_NORMAL);
#else
    CHECK(plain.kind == Kind::Posix);
    CHECK(std::string(clockwork::schedulingPolicyName(plain.policy)) == "SCHED_OTHER");
#endif

#if defined(__APPLE__)
    // CoreAudio's policy, as the plugin bridge's render thread is given it:
    // a millisecond's period, a quarter of it to compute, done within three quarters.
    clockwork::RealtimeStatus status{};
    clockwork::ThreadScheduling constrained{};
    std::thread([&] {
        status = clockwork::elevateCurrentThreadToRealtime(0.001).status;
        constrained = clockwork::currentThreadScheduling();
    }).join();
    REQUIRE(status == clockwork::RealtimeStatus::Applied);
    REQUIRE(constrained.kind == Kind::TimeConstrained);
    CHECK_THAT(constrained.period, Catch::Matchers::WithinAbs(0.001, 1e-6));
    // The kernel keeps the period and the deadline as asked; the computation it may raise to its own least.
    CHECK(constrained.computation >= 0.00025 - 1e-6);
    CHECK(constrained.computation <= constrained.constraint);
    CHECK_THAT(constrained.constraint, Catch::Matchers::WithinAbs(0.00075, 1e-6));
    CHECK(constrained.preemptible);
#endif
}

TEST_CASE("schedulingPolicyName names the POSIX policies", "[realtime]") {
#if defined(_WIN32)
    CHECK(std::string(clockwork::schedulingPolicyName(0)) == "another policy");
#else
    CHECK(std::string(clockwork::schedulingPolicyName(SCHED_RR)) == "SCHED_RR");
    CHECK(std::string(clockwork::schedulingPolicyName(SCHED_FIFO)) == "SCHED_FIFO");
    CHECK(std::string(clockwork::schedulingPolicyName(-1)) == "another policy");
#endif
}

// The CPU time a thread has had: what tells an audio callback that took long
// because it worked from one that took long because the thread was waiting or
// set aside (the [overrun] line). It grows while the thread works, and not
// while it sleeps.
TEST_CASE("currentThreadCpuSeconds grows with work and not with sleep", "[realtime]") {
    const double start = clockwork::currentThreadCpuSeconds();
#if defined(_WIN32)
    CHECK(start < 0.0);   // too coarse to say on Windows: it says so
#else
    REQUIRE(start >= 0.0);
    // Working: 10 ms of it, however busy the machine.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    volatile uint64_t spin = 0;
    while (clockwork::currentThreadCpuSeconds() - start < 0.010 && std::chrono::steady_clock::now() < deadline)
        for (int i = 0; i < 10000; ++i) spin = spin + 1;
    const double worked = clockwork::currentThreadCpuSeconds();
    CHECK(worked - start >= 0.010);
    // Asleep for 50 ms: barely any.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(clockwork::currentThreadCpuSeconds() - worked < 0.010);
#endif
}
