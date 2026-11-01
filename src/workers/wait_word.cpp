// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * wait_word.cpp — see wait_word.h. One branch per operating system's address
 * wait; the waiter count and the re-check are the same on all of them.
 */
#include "wait_word.h"
#include "platform.h"   // CLOCKWORK_LEAN_TARGET

#include <cerrno>
#include <climits>
#include <cmath>

#if defined(__EMSCRIPTEN__)
#  include <emscripten/threading.h>
#elif defined(__linux__)
#  include <ctime>
#  include <linux/futex.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <os/os_sync_wait_on_address.h>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "Synchronization.lib")
#  endif
#elif !defined(CLOCKWORK_LEAN_TARGET)
#  error "clockwork: no address wait for this platform (src/workers/wait_word.cpp)"
#endif

namespace clockwork {
namespace {

using std::chrono::duration_cast;
using std::chrono::nanoseconds;

// One wait on the operating system, for as long as `deadline` allows (none
// when `timed` is false). Returns when woken, when the value is not `old`,
// on a signal, or at the deadline; the caller re-checks.
void os_wait(ClockworkWaitWord& w, uint32_t old, bool timed, WaitClock::time_point deadline,
             WaitScope scope, WaitEvent event) {
    int64_t remaining_ns = -1;
    if (timed) {
        remaining_ns = duration_cast<nanoseconds>(deadline - WaitClock::now()).count();
        if (remaining_ns <= 0) return;
    }
#if defined(__EMSCRIPTEN__)
    (void)scope; (void)event;
    emscripten_futex_wait(&w.value, old, timed ? static_cast<double>(remaining_ns) / 1e6 : INFINITY);
#elif defined(__linux__)
    (void)event;
    const int op = scope == WaitScope::Shared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    timespec ts {};
    if (timed) {
        ts.tv_sec  = static_cast<time_t>(remaining_ns / 1'000'000'000);
        ts.tv_nsec = static_cast<long>(remaining_ns % 1'000'000'000);
    }
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&w.value), op, old, timed ? &ts : nullptr, nullptr, 0);
#elif defined(__APPLE__)
    (void)event;
    const auto flags = scope == WaitScope::Shared ? OS_SYNC_WAIT_ON_ADDRESS_SHARED : OS_SYNC_WAIT_ON_ADDRESS_NONE;
    if (timed)
        os_sync_wait_on_address_with_timeout(&w.value, old, sizeof(uint32_t), flags,
                                             OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<uint64_t>(remaining_ns));
    else
        os_sync_wait_on_address(&w.value, old, sizeof(uint32_t), flags);
#elif defined(_WIN32)
    // Milliseconds, rounded up, so a wait never ends before its deadline.
    const DWORD ms = timed ? static_cast<DWORD>((remaining_ns + 999'999) / 1'000'000) : INFINITE;
    if (scope == WaitScope::Shared && event) {
        WaitForSingleObject(static_cast<HANDLE>(event), ms);
    } else {
        uint32_t expected = old;
        WaitOnAddress(&w.value, &expected, sizeof(uint32_t), ms);
    }
#else
    // A lean target has one thread of control: nothing waits on another.
    (void)w; (void)old; (void)deadline; (void)scope; (void)event;
#endif
}

void os_wake_all(ClockworkWaitWord& w, WaitScope scope, WaitEvent event) {
#if defined(__EMSCRIPTEN__)
    (void)scope; (void)event;
    emscripten_futex_wake(&w.value, INT_MAX);
#elif defined(__linux__)
    (void)event;
    const int op = scope == WaitScope::Shared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE;
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&w.value), op, INT_MAX, nullptr, nullptr, 0);
#elif defined(__APPLE__)
    (void)event;
    os_sync_wake_by_address_all(&w.value, sizeof(uint32_t),
        scope == WaitScope::Shared ? OS_SYNC_WAKE_BY_ADDRESS_SHARED : OS_SYNC_WAKE_BY_ADDRESS_NONE);
#elif defined(_WIN32)
    if (scope == WaitScope::Shared && event) SetEvent(static_cast<HANDLE>(event));
    else WakeByAddressAll(&w.value);
#else
    (void)w; (void)scope; (void)event;
#endif
}

// Count this thread in, look, wait, count it out. The count goes in before
// the look, and the waker changes the value before it reads the count, both
// sequentially consistent: either the look sees the new value, or the waker
// sees the waiter and wakes it. The kernel's own compare closes the gap
// between the look and the sleep.
//
// Each side writes one word and then reads the other, which is safe only in a
// sequentially consistent order. A fence between the write and the read on
// each side makes that explicit. The Rust side (rust/clockwork-wait) has the
// same form, which loom model-checks, and either language may be on either
// side of a shared word.
bool wait_impl(ClockworkWaitWord& w, uint32_t old, bool timed, WaitClock::time_point deadline,
               WaitScope scope, WaitEvent event) {
    if (w.value.load(std::memory_order_seq_cst) != old) return true;
    if (timed && WaitClock::now() >= deadline) return false;
    w.waiters.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    while (w.value.load(std::memory_order_seq_cst) == old) {
        if (timed && WaitClock::now() >= deadline) break;
        os_wait(w, old, timed, deadline, scope, event);
    }
    w.waiters.fetch_sub(1, std::memory_order_seq_cst);
    return w.value.load(std::memory_order_seq_cst) != old;
}

}  // namespace

bool wait_until(ClockworkWaitWord& w, uint32_t old, WaitClock::time_point deadline,
                WaitScope scope, WaitEvent event) noexcept {
    return wait_impl(w, old, true, deadline, scope, event);
}

void wait(ClockworkWaitWord& w, uint32_t old, WaitScope scope, WaitEvent event) noexcept {
    wait_impl(w, old, false, WaitClock::time_point{}, scope, event);
}

void notify(ClockworkWaitWord& w, WaitScope scope, WaitEvent event) noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);   // between the caller's change and the look at the count
    if (w.waiters.load(std::memory_order_seq_cst) != 0) os_wake_all(w, scope, event);
}

}  // namespace clockwork
