// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * ThreadWakes.h — how often one of this process's threads woke, found by its
 * name (src/workers/thread_name.h).
 *
 * A thread that blocks on what changes wakes when it changes and not
 * otherwise; one that sleeps and looks again wakes every time it looks, with
 * nothing to do. No other test tells the two apart, so a case that pins a
 * thread as quiet counts the times the operating system woke it.
 *
 * Linux only: the count is the thread's voluntary context switches, which
 * /proc keeps per thread. Elsewhere there is no such count and a case that
 * needs one skips.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#if defined(__linux__)
#  include <filesystem>
#  include <fstream>
#endif

namespace thread_wakes {

// The named thread's wakes so far, or nothing if there is no such thread or
// no count to read.
inline std::optional<uint64_t> wakes(const std::string& name) {
#if defined(__linux__)
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& task : fs::directory_iterator("/proc/self/task", ec)) {
        std::ifstream comm(task.path() / "comm");
        std::string n;
        if (!std::getline(comm, n) || n != name) continue;
        std::ifstream status(task.path() / "status");
        const std::string key = "voluntary_ctxt_switches:";
        for (std::string line; std::getline(status, line);)
            if (line.rfind(key, 0) == 0) return std::stoull(line.substr(key.size()));
    }
#else
    (void)name;
#endif
    return std::nullopt;
}

inline bool countable() {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

// The named thread's wakes over `window`, from once it exists (it is waited
// for, up to ten seconds). Nothing if it never appears.
inline std::optional<uint64_t> over(const std::string& name, std::chrono::milliseconds window) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::optional<uint64_t> before;
    while (!(before = wakes(name)) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!before) return std::nullopt;
    std::this_thread::sleep_for(window);
    const auto after = wakes(name);
    if (!after) return std::nullopt;
    return *after - *before;
}

}  // namespace thread_wakes
