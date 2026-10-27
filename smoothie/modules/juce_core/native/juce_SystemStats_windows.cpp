/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

void Logger::outputDebugString (const String& text)
{
    OutputDebugString ((text + "\n").toWideCharPointer());
}

//==============================================================================
#ifdef JUCE_DLL_BUILD
 JUCE_API void* juceDLL_malloc (size_t sz)    { return std::malloc (sz); }
 JUCE_API void  juceDLL_free (void* block)    { std::free (block); }
#endif

//==============================================================================
#if JUCE_MINGW
 static uint64 getWindowsVersion()
 {
     auto filename = _T ("kernel32.dll");
     DWORD handle = 0;

     if (auto size = GetFileVersionInfoSize (filename, &handle))
     {
         HeapBlock<char> data (size);

         if (GetFileVersionInfo (filename, handle, size, data))
         {
             VS_FIXEDFILEINFO* info = nullptr;
             UINT verSize = 0;

             if (VerQueryValue (data, (LPCTSTR) _T ("\\"), (void**) &info, &verSize))
                 if (size > 0 && info != nullptr && info->dwSignature == 0xfeef04bd)
                     return ((uint64) info->dwFileVersionMS << 32) | (uint64) info->dwFileVersionLS;
         }
     }

     return 0;
 }
#else
 RTL_OSVERSIONINFOW getWindowsVersionInfo();
 RTL_OSVERSIONINFOW getWindowsVersionInfo()
 {
     RTL_OSVERSIONINFOW versionInfo = {};

     if (auto* moduleHandle = ::GetModuleHandleW (L"ntdll.dll"))
     {
         using RtlGetVersion = LONG (WINAPI*) (PRTL_OSVERSIONINFOW);

         if (auto* rtlGetVersion = (RtlGetVersion) ::GetProcAddress (moduleHandle, "RtlGetVersion"))
         {
             versionInfo.dwOSVersionInfoSize = sizeof (versionInfo);
             LONG STATUS_SUCCESS = 0;

             if (rtlGetVersion (&versionInfo) != STATUS_SUCCESS)
                 versionInfo = {};
         }
     }

     return versionInfo;
 }
#endif

SystemStats::OperatingSystemType SystemStats::getOperatingSystemType()
{
   #if JUCE_MINGW
    const auto v = getWindowsVersion();
    const auto major = (v >> 48) & 0xffff;
    const auto minor = (v >> 32) & 0xffff;
    const auto build = (v >> 16) & 0xffff;
   #else
    const auto versionInfo = getWindowsVersionInfo();
    const auto major = versionInfo.dwMajorVersion;
    const auto minor = versionInfo.dwMinorVersion;
    const auto build = versionInfo.dwBuildNumber;
   #endif

    jassert (major <= 10); // need to add support for new version!

    if (major == 10 && build >= 22000) return Windows11;
    if (major == 10)                   return Windows10;
    if (major == 6 && minor == 3)      return Windows8_1;
    if (major == 6 && minor == 2)      return Windows8_0;
    if (major == 6 && minor == 1)      return Windows7;
    if (major == 6 && minor == 0)      return WinVista;
    if (major == 5 && minor == 1)      return WinXP;
    if (major == 5 && minor == 0)      return Win2000;

    jassertfalse;
    return Windows;
}

//==============================================================================
uint32 juce_millisecondsSinceStartup() noexcept
{
    return (uint32) timeGetTime();
}

//==============================================================================
class HiResCounterHandler
{
public:
    HiResCounterHandler()
        : hiResTicksOffset (0)
    {
        // This macro allows you to override the default timer-period
        // used on Windows. By default this is set to 1, because that has
        // always been the value used in JUCE apps, and changing it could
        // affect the behaviour of existing code, but you may wish to make
        // it larger (or set it to 0 to use the system default) to make your
        // app less demanding on the CPU.
        // For more info, see win32 documentation about the timeBeginPeriod
        // function.
       #ifndef JUCE_WIN32_TIMER_PERIOD
        #define JUCE_WIN32_TIMER_PERIOD 1
       #endif

       #if JUCE_WIN32_TIMER_PERIOD > 0
        [[maybe_unused]] auto res = timeBeginPeriod (JUCE_WIN32_TIMER_PERIOD);
        jassert (res == TIMERR_NOERROR);
       #endif

        LARGE_INTEGER f;
        QueryPerformanceFrequency (&f);
        hiResTicksPerSecond = f.QuadPart;
        hiResTicksScaleFactor = 1000.0 / (double) hiResTicksPerSecond;
    }

    inline int64 getHighResolutionTicks() noexcept
    {
        LARGE_INTEGER ticks;
        QueryPerformanceCounter (&ticks);
        return ticks.QuadPart + hiResTicksOffset;
    }

    inline double getMillisecondCounterHiRes() noexcept
    {
        return (double) getHighResolutionTicks() * hiResTicksScaleFactor;
    }

    int64 hiResTicksPerSecond, hiResTicksOffset;
    double hiResTicksScaleFactor;
};

static HiResCounterHandler hiResCounterHandler;

int64  Time::getHighResolutionTicksPerSecond() noexcept  { return hiResCounterHandler.hiResTicksPerSecond; }
int64  Time::getHighResolutionTicks() noexcept           { return hiResCounterHandler.getHighResolutionTicks(); }
double Time::getMillisecondCounterHiRes() noexcept       { return hiResCounterHandler.getMillisecondCounterHiRes(); }

} // namespace juce
