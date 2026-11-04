// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * ResidentMemory.h — how much of the process is in RAM, where the platform
 * says: for the cases that hold a core to what it costs. 0 where it does not
 * say, which a case reads as "skip".
 *
 * A case that measures a cost measures it in a process of its own (ctest
 * runs each case so): memory an earlier case freed may still be resident,
 * and a core made into it costs nothing to the count.
 */
#pragma once
#include <cstddef>
#include <cstdio>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

namespace test_memory {

inline size_t residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info {};
    mach_msg_type_number_t n = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &n) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
#elif defined(__linux__)
    unsigned long pages = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    const int got = std::fscanf(f, "%lu %lu", &pages, &resident);
    std::fclose(f);
    return got == 2 ? resident * static_cast<size_t>(sysconf(_SC_PAGESIZE)) : 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc {};
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
#else
    return 0;
#endif
}

} // namespace test_memory
