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
/**
    The clocks the device layer reads. Smoothie keeps only the static
    counters its drivers use; JUCE's calendar Time object, its formatting and
    parsing, and the system-clock setter are gone.

    @tags{Core}
*/
class JUCE_API  Time  final
{
public:
    //==============================================================================
    /** Returns the current system time, as milliseconds since midnight on
        Jan 1st 1970 UTC.
    */
    static int64 currentTimeMillis() noexcept;

    /** Returns the number of millisecs since a fixed event (usually system startup).

        This returns a monotonically increasing value which is unaffected by changes to the
        system clock. It should be accurate to within a few millisecs, depending on platform
        hardware.
    */
    static uint32 getMillisecondCounter() noexcept;

    /** Returns the number of millisecs since a fixed event (usually system startup).

        This has the same function as getMillisecondCounter(), but returns a more accurate
        value, using a higher-resolution timer if one is available.
    */
    static double getMillisecondCounterHiRes() noexcept;

    //==============================================================================
    /** Returns the current high-resolution counter's tick-count.

        This is a similar idea to getMillisecondCounter(), but with a higher
        resolution.

        @see getHighResolutionTicksPerSecond, highResolutionTicksToSeconds,
             secondsToHighResolutionTicks
    */
    static int64 getHighResolutionTicks() noexcept;

    /** Returns the resolution of the high-resolution counter in ticks per second.
        @see getHighResolutionTicks, highResolutionTicksToSeconds,
             secondsToHighResolutionTicks
    */
    static int64 getHighResolutionTicksPerSecond() noexcept;

    /** Converts a number of high-resolution ticks into seconds.
        @see getHighResolutionTicks, getHighResolutionTicksPerSecond,
             secondsToHighResolutionTicks
    */
    static double highResolutionTicksToSeconds (int64 ticks) noexcept;

    /** Converts a number seconds into high-resolution ticks.
        @see getHighResolutionTicks, getHighResolutionTicksPerSecond,
             highResolutionTicksToSeconds
    */
    static int64 secondsToHighResolutionTicks (double seconds) noexcept;

private:
    Time() = delete; // uses only static methods
};

} // namespace juce
