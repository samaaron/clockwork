// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#include "DeviceLaneApartment.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#endif

namespace clockwork::device {

#ifdef _WIN32

DeviceLaneApartment::DeviceLaneApartment()
    : mWakeEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

DeviceLaneApartment::~DeviceLaneApartment() {
    if (mWakeEvent) CloseHandle(static_cast<HANDLE>(mWakeEvent));
}

void DeviceLaneApartment::enter() {
    // S_FALSE (already in it) counts: each success pairs with an
    // uninitialise.
    mEntered = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}

void DeviceLaneApartment::leave() {
    if (mEntered) CoUninitialize();
    mEntered = false;
}

void DeviceLaneApartment::wake() {
    SetEvent(static_cast<HANDLE>(mWakeEvent));
}

void DeviceLaneApartment::sleep() {
    // Back as soon as woken; meanwhile, any window message for this thread
    // (a driver's, COM's) is dispatched, as a message thread would.
    HANDLE wake = static_cast<HANDLE>(mWakeEvent);
    for (;;) {
        const DWORD r = MsgWaitForMultipleObjectsEx(1, &wake, INFINITE, QS_ALLINPUT,
                                                    MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0) return;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (r == WAIT_FAILED) return;   // no event to wait on: look again
    }
}

ProcessComApartment::ProcessComApartment() {
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    if (SUCCEEDED(CoIncrementMTAUsage(&cookie))) mCookie = cookie;
}

ProcessComApartment::~ProcessComApartment() {
    if (mCookie) CoDecrementMTAUsage(static_cast<CO_MTA_USAGE_COOKIE>(mCookie));
}

#else

DeviceLaneApartment::DeviceLaneApartment() = default;
DeviceLaneApartment::~DeviceLaneApartment() = default;
void DeviceLaneApartment::enter() {}
void DeviceLaneApartment::leave() {}

void DeviceLaneApartment::wake() {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mWoken = true;
    }
    mCv.notify_one();
}

void DeviceLaneApartment::sleep() {
    std::unique_lock<std::mutex> lock(mMutex);
    mCv.wait(lock, [this] { return mWoken; });
    mWoken = false;
}

ProcessComApartment::ProcessComApartment() = default;
ProcessComApartment::~ProcessComApartment() = default;

#endif

} // namespace clockwork::device
