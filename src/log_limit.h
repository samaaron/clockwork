// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * log_limit.h — how often one kind of log line is said.
 *
 * Trouble that repeats (junk on a port, a full ring, a queue refusing in
 * bursts) must not become a flood of log lines. How many times it happened
 * is a metric's job; these only choose which of the times are told. Each is
 * a member of whatever owns the trouble: a core's structure, a control
 * object, an engine. Any thread, lock-free.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace clockwork {

// By count: the first few times, or one time in so many.
class LogLimit {
public:
    // Said the first `n` times, then not again until reset.
    static constexpr LogLimit first(uint32_t n) { return LogLimit(n, 0); }
    // Said the first time and every `n`th time after.
    static constexpr LogLimit every(uint32_t n) { return LogLimit(0, n); }

    // Whether this time is one to say. Counts it.
    bool say() {
        const uint32_t seen = seen_.fetch_add(1, std::memory_order_relaxed);
        return every_ ? seen % every_ == 0 : seen < first_;
    }
    void reset() { seen_.store(0, std::memory_order_relaxed); }

    LogLimit(const LogLimit&) = delete;
    LogLimit& operator=(const LogLimit&) = delete;

private:
    constexpr LogLimit(uint32_t first, uint32_t every) : first_(first), every_(every) {}
    const uint32_t first_;
    const uint32_t every_;
    std::atomic<uint32_t> seen_{0};
};

// By time: lines at least `interval` apart, by a clock the caller reads and
// gives in the same units (block time in NTP units, a steady clock's
// nanoseconds). The first line is always said.
class LogSpacing {
public:
    explicit constexpr LogSpacing(uint64_t interval) : interval_(interval) {}

    // Whether a line at `now` is one to say. If two threads ask at once,
    // one is told yes.
    bool say(uint64_t now) {
        uint64_t last = last_.load(std::memory_order_relaxed);
        if (last != kNever && now - last < interval_) return false;
        return last_.compare_exchange_strong(last, now, std::memory_order_relaxed);
    }
    void reset() { last_.store(kNever, std::memory_order_relaxed); }

    LogSpacing(const LogSpacing&) = delete;
    LogSpacing& operator=(const LogSpacing&) = delete;

private:
    static constexpr uint64_t kNever = UINT64_MAX;
    const uint64_t interval_;
    std::atomic<uint64_t> last_{kNever};
};

} // namespace clockwork
