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

#if JUCE_BELA
extern "C" int cobalt_thread_mode();
#endif

namespace juce
{

//==============================================================================
void Logger::outputDebugString (const String& text)
{
    std::cerr << text << std::endl;
}

//==============================================================================
uint32 juce_millisecondsSinceStartup() noexcept
{
    return (uint32) (Time::getHighResolutionTicks() / 1000);
}

int64 Time::getHighResolutionTicks() noexcept
{
    timespec t;

   #if JUCE_BELA
    if (cobalt_thread_mode() == 0x200 /*XNRELAX*/)
        clock_gettime (CLOCK_MONOTONIC, &t);
    else
        __wrap_clock_gettime (CLOCK_MONOTONIC, &t);
   #else
    clock_gettime (CLOCK_MONOTONIC, &t);
   #endif

    return (t.tv_sec * (int64) 1000000) + (t.tv_nsec / 1000);
}

int64 Time::getHighResolutionTicksPerSecond() noexcept
{
    return 1000000;  // (microseconds)
}

double Time::getMillisecondCounterHiRes() noexcept
{
    return (double) getHighResolutionTicks() * 0.001;
}

JUCE_API bool JUCE_CALLTYPE juce_isRunningUnderDebugger() noexcept
{
   #if JUCE_BSD
    int mib[] =
    {
        CTL_KERN,
        KERN_PROC,
        KERN_PROC_PID,
        ::getpid()
    };
    struct kinfo_proc info;
    auto infoSize = sizeof (info);
    auto result = sysctl (mib, numElementsInArray (mib), &info, &infoSize, nullptr, 0);
    return result == 0 ? ((info.ki_flag & P_TRACED) != 0) : false;
   #else
    return readPosixConfigFileValue ("/proc/self/status", "TracerPid").getIntValue() > 0;
   #endif
}

} // namespace juce
