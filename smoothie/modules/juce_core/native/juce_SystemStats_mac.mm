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

ScopedAutoReleasePool::ScopedAutoReleasePool()
{
    pool = [[NSAutoreleasePool alloc] init];
}

ScopedAutoReleasePool::~ScopedAutoReleasePool()
{
    [((NSAutoreleasePool*) pool) release];
}

//==============================================================================
void Logger::outputDebugString (const String& text)
{
    // Would prefer to use std::cerr here, but avoiding it for
    // the moment, due to clang JIT linkage problems.
    fputs (text.toRawUTF8(), stderr);
    fputs ("\n", stderr);
    fflush (stderr);
}

//==============================================================================
/*  NB: these are kept outside the HiResCounterInfo struct and initialised to 1 to avoid
    division-by-zero errors if some other static constructor calls us before this file's
    static constructors have had a chance to fill them in correctly..
*/
static uint64 hiResCounterNumerator = 0, hiResCounterDenominator = 1;

class HiResCounterInfo
{
public:
    HiResCounterInfo()
    {
        mach_timebase_info_data_t timebase;
        (void) mach_timebase_info (&timebase);

        if (timebase.numer % 1000000 == 0)
        {
            hiResCounterNumerator   = timebase.numer / 1000000;
            hiResCounterDenominator = timebase.denom;
        }
        else
        {
            hiResCounterNumerator   = timebase.numer;
            hiResCounterDenominator = timebase.denom * (uint64) 1000000;
        }

        highResTimerFrequency = (timebase.denom * (uint64) 1000000000) / timebase.numer;
        highResTimerToMillisecRatio = (double) hiResCounterNumerator / (double) hiResCounterDenominator;
    }

    uint32 millisecondsSinceStartup() const noexcept
    {
        return (uint32) ((mach_absolute_time() * hiResCounterNumerator) / hiResCounterDenominator);
    }

    double getMillisecondCounterHiRes() const noexcept
    {
        return (double) mach_absolute_time() * highResTimerToMillisecRatio;
    }

    int64 highResTimerFrequency;

private:
    double highResTimerToMillisecRatio;
};

static HiResCounterInfo hiResCounterInfo;

uint32 juce_millisecondsSinceStartup() noexcept         { return hiResCounterInfo.millisecondsSinceStartup(); }
double Time::getMillisecondCounterHiRes() noexcept      { return hiResCounterInfo.getMillisecondCounterHiRes(); }
int64  Time::getHighResolutionTicksPerSecond() noexcept { return hiResCounterInfo.highResTimerFrequency; }
int64  Time::getHighResolutionTicks() noexcept          { return (int64) mach_absolute_time(); }

// The main bundle is an app extension when its path ends in .appex, on both
// macOS and iOS.
bool juce_mainBundleIsAppExtension();
bool juce_mainBundleIsAppExtension()
{
    return [[[NSBundle mainBundle] bundlePath] hasSuffix: @".appex"];
}

} // namespace juce
