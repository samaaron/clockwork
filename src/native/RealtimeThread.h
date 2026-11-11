// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
#pragma once

// Audio-thread realtime promotion. The ALSA driver (Smoothie's, from JUCE)
// runs its audio thread at SCHED_OTHER (its "high" priority maps to no
// realtime guarantee on Linux), so under CPU contention the callback fires late
// and the scheduler accrues "lates". This helper lifts the *calling* thread to
// SCHED_RR — call it from the audio callback so the audio thread itself is
// promoted.

namespace clockwork {

enum class RealtimeStatus {
    Applied,       // the calling thread is now running realtime (SCHED_RR)
    NotPermitted,  // the OS denied it (no rtprio permission) — thread unchanged
    Failed,        // an unexpected error — thread unchanged
    NotSupported,  // platform needs no action here (non-Linux)
};

struct RealtimeResult {
    RealtimeStatus status;
    int policy;    // resulting scheduling policy (the *current* one on failure)
    int priority;  // resulting priority (the *current* one on failure)
    int error;     // the pthread_setschedparam error code on failure, else 0
};

// Attempt to raise the CURRENT thread to realtime (SCHED_RR) at a modest
// priority. If the process lacks rtprio permission the call is a no-op that
// leaves the thread exactly as it was. Never throws, never blocks. Idempotent.
RealtimeResult elevateCurrentThreadToRealtime();

// The same for a thread that is not the device's callback but must keep up
// with it: the plugin bridge's render thread, woken once per block. On macOS
// this applies the time-constraint policy, which is what CoreAudio gives its
// own callback thread and the only way a thread in a process that owns no
// audio device gets scheduled like one; `periodSeconds` is the block period.
// On Linux and Windows it is the call above.
RealtimeResult elevateCurrentThreadToRealtime(double periodSeconds);

// How the calling thread is scheduled now, as the OS has it: what the audio
// thread was given, said as each device starts. Never blocks, never allocates.
struct ThreadScheduling {
    enum class Kind {
        TimeConstrained,  // macOS's time-constraint policy, CoreAudio's for its I/O threads
        Posix,            // a POSIX policy and priority (Linux; a macOS thread without the time constraint)
        Windows,          // a Win32 thread priority
        Unknown,          // the OS would not say
    };
    Kind kind = Kind::Unknown;
    // TimeConstrained, in seconds: the thread runs once a period, needs the
    // computation, and must be done within the constraint of its start.
    double period = 0.0, computation = 0.0, constraint = 0.0;
    bool preemptible = false;
    int policy = 0;     // Posix: SCHED_OTHER, SCHED_FIFO, SCHED_RR
    int priority = 0;   // Posix: the policy's priority; Windows: GetThreadPriority
};
ThreadScheduling currentThreadScheduling();

// A POSIX policy's name ("SCHED_RR"), or "another policy".
const char* schedulingPolicyName(int policy);

// The CPU time the calling thread has had, in seconds from an origin of its
// own; negative where the platform can't say finely (Windows). Read around a
// piece of work, it tells the work from time the thread spent waiting or set
// aside by the scheduler. A non-blocking call, fit for the audio thread.
double currentThreadCpuSeconds();

}  // namespace clockwork
