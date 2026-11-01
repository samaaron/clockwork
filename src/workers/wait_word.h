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
 * changes the value, each with a sequentially consistent fence between its
 * write and its read, so one of them always sees the other. A wake with no
 * waiter is therefore two atomic operations and a fence, and no system call,
 * which is what lets the audio thread wake the control pass every block. With
 * a waiter it is one non-blocking system call.
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
#include <stop_token>

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

// Lets a worker ask one source of work to wake it through the worker's word.
// Until the worker asks, each ring costs the source a fence and a load; once
// it asks, the next ring wakes it, once. Use it for a source that stays busy
// whether or not the worker wants it, such as a stream of arrivals into a
// ring the worker cannot drain while its output is full.
//
// The worker asks and then looks at what the source changes; the source
// changes it and then rings, with a fence between on each side, so one of
// them always sees the other. After a new ask the worker must look once more
// before it sleeps, because a ring between its last look and the ask found
// nothing asked.
struct WakeRequest {
    std::atomic<bool> asked{false};

    // The worker asks. Returns true if the ask is new, and then the worker
    // must look again before it sleeps.
    bool ask() noexcept {
        const bool had = asked.exchange(true, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return !had;
    }
    // The worker no longer wants the wake.
    void withdraw() noexcept { asked.store(false, std::memory_order_seq_cst); }
    // The source rings after its change. Never blocks.
    void ring(ClockworkWaitWord& word) noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (asked.load(std::memory_order_relaxed) && asked.exchange(false, std::memory_order_seq_cst))
            bump(word);
    }
};

// What a worker's pass returns: run again at once, sleep until a time, or
// sleep until the word changes.
struct NextPass {
    enum class Kind : uint8_t { Now, Until, Woken };
    Kind kind;
    WaitClock::time_point at;
    static NextPass now() { return { Kind::Now, {} }; }
    static NextPass until(WaitClock::time_point t) { return { Kind::Until, t }; }
    static NextPass woken() { return { Kind::Woken, {} }; }
};

// Runs a worker thread's loop until `stop`: run `pass`, then sleep on `w` as
// the pass asks. To give the worker something to do, change what the pass
// looks at and then bump `w`; a stop bumps it too.
//
// THE ORDER IS WHAT LOSES NO WAKE. The loop reads the word first, before it
// checks the stop and before the pass runs, and sleeps on that reading, so a
// stop or a bump at any moment after the read ends the sleep. Read after the
// check, a stop or a bump in between would be slept through. Every worker
// loop in clockwork uses this function, so the order is written once.
template <class Pass>
void run_worker(ClockworkWaitWord& w, const std::stop_token& stop, Pass&& pass) {
    const std::stop_callback wake(stop, [&w] { bump(w); });
    for (;;) {
        const uint32_t seen = w.value.load(std::memory_order_seq_cst);
        if (stop.stop_requested()) return;
        const NextPass next = pass();
        switch (next.kind) {
            case NextPass::Kind::Now:   break;
            case NextPass::Kind::Until: wait_until(w, seen, next.at); break;
            case NextPass::Kind::Woken: wait(w, seen); break;
        }
    }
}

// Block until `deadline`, or until `stop` is requested. True if stop was
// requested. For work that is due at a time, such as a sampling interval, on
// a thread that must still leave the moment it is asked to.
inline bool sleep_until(const std::stop_token& stop, WaitClock::time_point deadline) {
    ClockworkWaitWord w;
    const std::stop_callback wake(stop, [&w] { bump(w); });
    while (!stop.stop_requested() && WaitClock::now() < deadline) wait_until(w, 0, deadline);
    return stop.stop_requested();
}

}  // namespace clockwork
