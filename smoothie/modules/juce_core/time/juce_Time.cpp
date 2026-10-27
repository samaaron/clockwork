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

//==============================================================================
int64 Time::currentTimeMillis() noexcept
{
   #if JUCE_WINDOWS && ! JUCE_MINGW
    struct _timeb t;
    _ftime_s (&t);
    return ((int64) t.time) * 1000 + t.millitm;
   #else
    struct timeval tv;
    gettimeofday (&tv, nullptr);
    return ((int64) tv.tv_sec) * 1000 + tv.tv_usec / 1000;
   #endif
}

//==============================================================================
uint32 juce_millisecondsSinceStartup() noexcept;

static Atomic<uint32> lastMSCounterValue { (uint32) 0x0 };

uint32 Time::getMillisecondCounter() noexcept
{
    auto now = juce_millisecondsSinceStartup();

    if (now < lastMSCounterValue.get())
    {
        // in multi-threaded apps this might be called concurrently, so
        // make sure that our last counter value only increases and doesn't
        // go backwards..
        if (now < lastMSCounterValue.get() - (uint32) 1000)
            lastMSCounterValue = now;
    }
    else
    {
        lastMSCounterValue = now;
    }

    return now;
}

//==============================================================================
double Time::highResolutionTicksToSeconds (const int64 ticks) noexcept
{
    return (double) ticks / (double) getHighResolutionTicksPerSecond();
}

int64 Time::secondsToHighResolutionTicks (const double seconds) noexcept
{
    return (int64) (seconds * (double) getHighResolutionTicksPerSecond());
}

} // namespace juce
