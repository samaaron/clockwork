// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * DeviceLaneApartment.h — the device lane's thread, as the device layer
 * needs it, and how it is woken.
 *
 * Every device the engine opens is opened, driven and closed on its device
 * lane (ClockworkEngine::deviceTaskLoop). On Windows the device layer is COM,
 * and its drivers expect what JUCE's message thread used to give them: one
 * thread in a single-threaded apartment (an ASIO driver is an apartment-
 * threaded COM object with no proxies, usable only from the thread that made
 * it) that dispatches its window messages while it waits (some drivers run
 * windows of their own on it). The lane is that thread. Anything else that
 * makes a passing call into Windows' audio APIs (free-threaded) finds the
 * process's multithreaded apartment kept for it (ProcessComApartment).
 *
 * Waking the lane takes no allocation and no wait on the lane, so an OS or
 * driver callback — a driver's own audio thread included — can say that the
 * devices changed (ClockworkEngine::devicesChanged). A wake is kept until the
 * lane next sleeps, so none is lost between its last look and its sleep.
 * Everywhere but Windows the apartment is nothing and the wait is plain.
 */
#pragma once

#include <condition_variable>
#include <mutex>

namespace clockwork::device {

class DeviceLaneApartment {
public:
    DeviceLaneApartment();
    ~DeviceLaneApartment();
    DeviceLaneApartment(const DeviceLaneApartment&) = delete;
    DeviceLaneApartment& operator=(const DeviceLaneApartment&) = delete;

    // On the lane, first and last: into its apartment, and out of it.
    void enter();
    void leave();

    // Any thread: the lane looks again. Kept until the lane next sleeps.
    void wake();

    // On the lane: until woken, dispatching the thread's window messages
    // meanwhile on Windows.
    void sleep();

private:
#ifdef _WIN32
    void* mWakeEvent = nullptr;   // auto-reset: a wake is kept until taken
    bool  mEntered = false;
#else
    std::mutex              mMutex;
    std::condition_variable mCv;
    bool                    mWoken = false;
#endif
};

// The process's multithreaded apartment, kept while one exists: a thread
// that has not chosen an apartment can then call Windows' free-threaded
// audio APIs (WASAPI, the MMDevice API). Nothing elsewhere.
class ProcessComApartment {
public:
    ProcessComApartment();
    ~ProcessComApartment();
    ProcessComApartment(const ProcessComApartment&) = delete;
    ProcessComApartment& operator=(const ProcessComApartment&) = delete;

private:
    void* mCookie = nullptr;
};

} // namespace clockwork::device
