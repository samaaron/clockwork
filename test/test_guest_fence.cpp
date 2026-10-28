// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_guest_fence.cpp — the guest keeps to the memory it was handed.
 *
 * A guest is native code called on the engine thread with pointers into the
 * arena, and nothing in a plain build stops it reading or writing the rest of
 * the process. Under AddressSanitizer there is a fence (src/guest_fence.h):
 * while a guest entry point is open, every arena region the guest was not
 * handed is poisoned, so a stray access fails the run with the address and
 * the stack, the way any other sanitizer finding does.
 *
 * The proof has to die to pass. A poisoned access is a sanitizer report and
 * an exit, not an exception a test can catch, so each case here runs the
 * placeholder DSP in a child process (this binary again, on a hidden case)
 * and reads what the child's death says. One child pokes a region the guest
 * must never touch and must be reported; one pokes inside its own window and
 * must come back clean, which is what proves the fence is not simply killing
 * everything.
 *
 * The cases that spawn exist only in a sanitizer build; the hidden child case
 * is always compiled, so a plain build still proves the poke verb reaches the
 * address it is given.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "audio_processor.h"
#include "clockwork_arena.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__SANITIZE_ADDRESS__)
#define CLOCKWORK_FENCE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CLOCKWORK_FENCE_ASAN 1
#endif
#endif
#ifndef CLOCKWORK_FENCE_ASAN
#define CLOCKWORK_FENCE_ASAN 0
#endif

#if CLOCKWORK_FENCE_ASAN && !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr const char* kChildCase = "guest fence child: the guest pokes the region it was told to";
constexpr const char* kRegionVar = "CLOCKWORK_FENCE_REGION";

// Where a region of the arena that is up starts, by its stable id.
uint8_t* regionBase(uint32_t id) {
    auto* base = static_cast<uint8_t*>(get_shared_memory_base());
    REQUIRE(base != nullptr);
    const auto* h = reinterpret_cast<const ClockworkArenaHeader*>(base);
    const ClockworkArenaEntry* e = clockwork_arena_find(h, id);
    REQUIRE(e != nullptr);
    return base + e->offset;
}

}  // namespace

// The child. Boots the fixture, tells the placeholder DSP to poke one byte at
// the start of the region named in the environment, from inside dsp_osc, and
// ticks once so the message is delivered. Hidden: it is run by name, from the
// cases below, never by the suite.
TEST_CASE("guest fence child: the guest pokes the region it was told to", "[.fence-child]") {
    const char* which = std::getenv(kRegionVar);
    REQUIRE(which != nullptr);
    const auto id = static_cast<uint32_t>(std::atoi(which));

    lanes_test::boot();
    lanes_test::drainRt();

    const auto at = reinterpret_cast<uint64_t>(regionBase(id));
    const auto poke = osc_test::message("/dummy/poke",
                                        static_cast<int32_t>(at >> 32),
                                        static_cast<int32_t>(at & 0xFFFFFFFFu));
    REQUIRE(lanes_test::ingress(poke.ptr(), poke.size(), 0x9e62));
    lanes_test::tickUntil(2, nullptr);
}

#if CLOCKWORK_FENCE_ASAN && !defined(_WIN32)

namespace {

struct ChildRun {
    int         status = -1;    // what waitpid saw
    std::string stderrText;
};

// This binary again, on the hidden case, with the region to poke in its
// environment and its stderr captured: a sanitizer writes its report there.
ChildRun runChild(uint32_t region) {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);   // the sanitizer's report
        dup2(fds[1], STDOUT_FILENO);   // and the child's own test output, kept out of the suite's
        close(fds[1]);
        const std::string value = std::to_string(region);
        setenv(kRegionVar, value.c_str(), 1);
        execl(CLOCKWORK_TESTS_BINARY, "clockwork_tests", kChildCase, nullptr);
        _exit(127);
    }
    close(fds[1]);
    ChildRun run;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        run.stderrText.append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    waitpid(pid, &run.status, 0);
    return run;
}

}  // namespace

TEST_CASE("guest fence: a guest that writes a region it was not handed is reported",
          "[guest][fence]") {
    // The metrics block: clockwork's, read by clients, never handed to a
    // guest. The fence must hold it while the guest is running.
    const ChildRun run = runChild(CLOCKWORK_ARENA_CLOCKWORK_METRICS);
    INFO(run.stderrText);
    CHECK(run.stderrText.find("use-after-poison") != std::string::npos);
    const bool exitedClean = WIFEXITED(run.status) && WEXITSTATUS(run.status) == 0;
    CHECK_FALSE(exitedClean);
}

TEST_CASE("guest fence: a guest that writes its own window is left alone",
          "[guest][fence]") {
    // The same poke into DspConfig::shm_window, which is the guest's to
    // write: no report, a clean exit.
    const ChildRun run = runChild(CLOCKWORK_ARENA_GUEST_WINDOW);
    INFO(run.stderrText);
    CHECK(run.stderrText.find("AddressSanitizer") == std::string::npos);
    CHECK(WIFEXITED(run.status));
    CHECK(WEXITSTATUS(run.status) == 0);
}

#endif  // CLOCKWORK_FENCE_ASAN && !_WIN32
