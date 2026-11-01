// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * thread_name.cpp — see thread_name.h.
 */
#include "thread_name.h"

#include <cstring>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#  include <pthread.h>
#endif

namespace clockwork {

void name_this_thread(const char* name) noexcept {
    char cut[16] = {};
    std::strncpy(cut, name, sizeof cut - 1);
#if defined(__EMSCRIPTEN__)
    (void)cut;
#elif defined(_WIN32)
    // Looked up by name: SetThreadDescription arrived in Windows 10 1607,
    // and a missing import would stop the process loading at all.
    using Describe = HRESULT (WINAPI*)(HANDLE, PCWSTR);
    static const auto describe = reinterpret_cast<Describe>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (!describe) return;
    wchar_t wide[16] = {};
    for (size_t i = 0; cut[i]; ++i) wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(cut[i]));
    describe(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    pthread_setname_np(cut);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), cut);
#else
    (void)cut;
#endif
}

}  // namespace clockwork
