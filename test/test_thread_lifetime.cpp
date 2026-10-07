// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_thread_lifetime.cpp — a thread's owner, dropped, takes the thread with it.
 *
 * Every engine thread is a std::jthread whose loop reads a stop_token and
 * registers a stop_callback that does its own wake, so an owner torn down by
 * any path — stop(), an early return, an exception — joins its thread rather
 * than calling std::terminate (a std::thread member still joinable in its
 * destructor) or leaving it running against freed state (a detached one).
 * scripts/check-threads.sh keeps every thread in that shape; this proves the
 * shape holds for the one most often parked in a wait: the ring reader.
 *
 * The drop happens in a forked child so a std::terminate, were it to come
 * back, is a signal the parent reads rather than the end of this suite.
 */
#if !defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "workers/RingReader.h"

namespace {

// What the child's exit became: 0 for a clean return, the signal number for a
// signal, -1 for a timeout.
int dropInChild(void (*body)()) {
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        body();
        _exit(0);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return WTERMSIG(status);
            return -2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

std::atomic<uint32_t> g_wake{0};

// The reader parked on its wake word, dropped without stop().
void dropParkedReader() {
    RingReader reader("dropped-while-parked");
    reader.setWake(&g_wake);
    reader.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));   // asleep in the wait by now
}

// The reader parked by pause(), dropped without resume() or stop().
void dropPausedReader() {
    RingReader reader("dropped-while-paused");
    reader.setWake(&g_wake);
    reader.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)reader.pause();
}

}  // namespace

// Under ThreadSanitizer the forked child is not a program it can follow (the
// runtime warns and its reports in the child read as a failure), so these two
// run in the plain, ASan and UBSan builds and are skipped there.
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
#define CLOCKWORK_SKIP_UNDER_TSAN() SKIP("fork is outside what ThreadSanitizer can follow")
#else
#define CLOCKWORK_SKIP_UNDER_TSAN() ((void)0)
#endif

TEST_CASE("a ring reader dropped while parked on its wake word joins, never terminates",
          "[threads][lifetime]") {
    CLOCKWORK_SKIP_UNDER_TSAN();
    const int outcome = dropInChild(&dropParkedReader);
    INFO("child outcome " << outcome << " (0 = joined and returned; " << SIGABRT
                          << " = std::terminate; -1 = hung)");
    CHECK(outcome == 0);
}

TEST_CASE("a ring reader dropped while paused is released and joined",
          "[threads][lifetime]") {
    CLOCKWORK_SKIP_UNDER_TSAN();
    const int outcome = dropInChild(&dropPausedReader);
    INFO("child outcome " << outcome);
    CHECK(outcome == 0);
}

#endif  // !_WIN32
