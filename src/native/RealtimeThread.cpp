// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
#include "RealtimeThread.h"

#if defined(__linux__)
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt")
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace clockwork {

#if defined(__linux__)

RealtimeResult elevateCurrentThreadToRealtime() {
    // Capture the current policy first so a denied request can report the
    // unchanged state.
    int currentPolicy = SCHED_OTHER;
    sched_param currentParam{};
    pthread_getschedparam(pthread_self(), &currentPolicy, &currentParam);

    constexpr int kPolicy = SCHED_RR;
    const int lo = sched_get_priority_min(kPolicy);
    const int hi = sched_get_priority_max(kPolicy);

    // A modest realtime priority (low quarter of the RR range): above ordinary
    // SCHED_OTHER work, but below where a system audio server (PipeWire/JACK) or
    // kernel realtime threads sit, so those are not starved.
    const int priority = lo + (hi - lo) / 4;

    sched_param param{};
    param.sched_priority = priority;

    // pthread_setschedparam is atomic: it either applies policy+priority wholly
    // or changes nothing. It returns the error code directly (not via errno).
    const int rc = pthread_setschedparam(pthread_self(), kPolicy, &param);
    if (rc == 0)
        return {RealtimeStatus::Applied, kPolicy, priority, 0};

    const RealtimeStatus status =
        (rc == EPERM) ? RealtimeStatus::NotPermitted : RealtimeStatus::Failed;
    return {status, currentPolicy, currentParam.sched_priority, rc};
}

#elif defined(_WIN32)

RealtimeResult elevateCurrentThreadToRealtime() {
    // Windows audio threads are NOT protected by default: JUCE's DirectSound
    // backend polls from an ordinary Priority::highest juce::Thread, and even
    // ASIO driver threads aren't necessarily MMCSS-registered. Under CPU load
    // (a compile, a browser) the callback gets preempted for multi-ms bursts
    // → DSP overruns → audible stutter with no callback gap.
    //
    // Two independent measures; either alone is a win:
    //  * MMCSS "Pro Audio" registration — the OS-blessed audio-thread
    //    protection (the same scheduler class the WASAPI engine uses).
    //  * THREAD_PRIORITY_TIME_CRITICAL — the classic boost the previous
    //    engine always applied on Windows.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss)
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    const BOOL boosted =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const int resulting = GetThreadPriority(GetCurrentThread());

    // `policy` reports MMCSS engagement (1 = registered); `priority` the
    // resulting Win32 thread priority.
    if (mmcss || boosted)
        return {RealtimeStatus::Applied, mmcss ? 1 : 0, resulting, 0};
    return {RealtimeStatus::Failed, 0, resulting,
            static_cast<int>(GetLastError())};
}

#else  // other platforms: the audio thread keeps its existing OS/JUCE behaviour.

RealtimeResult elevateCurrentThreadToRealtime() {
    return {RealtimeStatus::NotSupported, 0, 0, 0};
}

#endif

#if defined(__APPLE__)

// THREAD_TIME_CONSTRAINT_POLICY: the policy CoreAudio sets on its own I/O
// thread. Period is the block, computation the share of it the render is
// expected to take, constraint the deadline; preemptible because the render
// is not the whole story of the block. The numbers are in Mach absolute
// time units, hence the timebase conversion.
RealtimeResult elevateCurrentThreadToRealtime(double periodSeconds) {
    if (!(periodSeconds > 0.0)) periodSeconds = 0.0027;  // 128 frames at 48 kHz
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    const double nsPerTick = tb.numer ? double(tb.numer) / double(tb.denom) : 1.0;
    const double periodTicks = periodSeconds * 1e9 / nsPerTick;
    thread_time_constraint_policy_data_t policy{};
    policy.period      = static_cast<uint32_t>(periodTicks);
    policy.computation = static_cast<uint32_t>(periodTicks * 0.25);
    policy.constraint  = static_cast<uint32_t>(periodTicks * 0.75);
    policy.preemptible = 1;
    const kern_return_t kr = thread_policy_set(
        pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) return {RealtimeStatus::Failed, 0, 0, static_cast<int>(kr)};
    return {RealtimeStatus::Applied, 0, 0, 0};
}

#else

RealtimeResult elevateCurrentThreadToRealtime(double) {
    return elevateCurrentThreadToRealtime();
}

#endif

}  // namespace clockwork
