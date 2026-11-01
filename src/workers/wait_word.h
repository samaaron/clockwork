// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * wait_word.h — a word a thread blocks on until it changes, with a deadline.
 *
 * What std::atomic::wait is, with the two things it leaves out: a deadline,
 * and a wake that crosses a process boundary. Every wait in clockwork that is
 * for something another thread does — the control pass for work, a pause for
 * its acknowledgement, a boot for its first block — blocks here on the word
 * that changes, and nothing sleeps for a fixed time and looks again.
 *
 * Built on the operating system's address wait, as the standard library's
 * own atomic wait is: futex on Linux (Android and Bela too),
 * os_sync_wait_on_address on Apple (macOS 14.4, iOS 17.4), WaitOnAddress on
 * Windows, and the wasm atomic wait in a browser.
 *
 * WAKING IS CHEAP WHEN NOBODY WAITS. A waiter counts itself in `waiters`
 * before it looks at the value, and a waker looks at `waiters` after it
 * changes the value; both sides use sequentially consistent operations, so
 * one of them always sees the other. A wake with no waiter is therefore two
 * atomic operations and no system call, which is what lets the audio thread
 * wake the control pass every block. With a waiter it is one non-blocking
 * system call.
 *
 * SHARED. A word in memory another process maps (a shared-memory segment)
 * wakes a waiter there with WaitScope::Shared. Windows has no cross-process
 * address wait, so a shared word there comes with an event handle each
 * process holds, made by whoever creates the word; elsewhere the handle is
 * unused.
 *
 * A wait may return early (a signal, a spurious wake); every caller re-checks
 * what it is waiting for, which wait_for_condition does for it.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// The word, and the count of threads blocked on it. Plain 32-bit atomics, so
// it may sit in shared memory and be read by another build.
struct ClockworkWaitWord {
    std::atomic<uint32_t> value{0};
    std::atomic<uint32_t> waiters{0};
};
static_assert(sizeof(ClockworkWaitWord) == 8, "ClockworkWaitWord is a wire format in shared memory");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "a wait word must be lock-free to be shared");

namespace clockwork {

enum class WaitScope : uint8_t {
    Process,   // waiters and wakers are threads of this process
    Shared,    // the word is in shared memory, and a waiter may be in another process
};

// A shared word's event on Windows (a HANDLE); unused elsewhere.
using WaitEvent = void*;

using WaitClock = std::chrono::steady_clock;

// Block until the value is not `old`, or `deadline` passes. True if the
// value changed. A deadline already past is a look, not a wait.
bool wait_until(ClockworkWaitWord& w, uint32_t old, WaitClock::time_point deadline,
                WaitScope scope = WaitScope::Process, WaitEvent event = nullptr) noexcept;

// Block until the value is not `old`.
void wait(ClockworkWaitWord& w, uint32_t old,
          WaitScope scope = WaitScope::Process, WaitEvent event = nullptr) noexcept;

// Wake every waiter: the caller has already changed the value (or something
// the waiters re-check). Any thread, the audio thread included; never blocks.
void notify(ClockworkWaitWord& w,
            WaitScope scope = WaitScope::Process, WaitEvent event = nullptr) noexcept;

// Add one to the value and wake every waiter. Any thread; never blocks.
inline void bump(ClockworkWaitWord& w,
                 WaitScope scope = WaitScope::Process, WaitEvent event = nullptr) noexcept {
    w.value.fetch_add(1, std::memory_order_seq_cst);
    notify(w, scope, event);
}

// Block until `done()` is true or `deadline` passes, looking again each time
// the word changes. For a handshake: whoever changes what `done` reads bumps
// the word after. True if `done()` came true.
template <class Done>
bool wait_for_condition(ClockworkWaitWord& w, Done&& done, WaitClock::time_point deadline,
                        WaitScope scope = WaitScope::Process, WaitEvent event = nullptr) {
    for (;;) {
        const uint32_t seen = w.value.load(std::memory_order_seq_cst);
        if (done()) return true;
        if (WaitClock::now() >= deadline) return false;
        wait_until(w, seen, deadline, scope, event);
    }
}

// The same with no deadline.
template <class Done>
void wait_for_condition(ClockworkWaitWord& w, Done&& done,
                        WaitScope scope = WaitScope::Process, WaitEvent event = nullptr) {
    for (;;) {
        const uint32_t seen = w.value.load(std::memory_order_seq_cst);
        if (done()) return;
        wait(w, seen, scope, event);
    }
}

}  // namespace clockwork
