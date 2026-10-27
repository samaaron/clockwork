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
    The few facts about the host the device layer asks for. Smoothie keeps
    only what its drivers use: the JUCE version string, the device
    manufacturer on Android, and whether the process is an app extension on
    Apple platforms. Windows 10 and later is assumed, so there is no
    operating-system version query.

    @tags{Core}
*/
class JUCE_API  SystemStats  final
{
public:
    //==============================================================================
    /** Returns the current version of JUCE,
        See also the JUCE_VERSION, JUCE_MAJOR_VERSION and JUCE_MINOR_VERSION macros.
    */
    static String getJUCEVersion();

    /** Returns the device manufacturer (Android). */
    static String getDeviceManufacturer();

    //==============================================================================
    /** Returns true if this process is an app extension (macOS and iOS). */
    static bool isRunningInAppExtensionSandbox() noexcept;

private:
    SystemStats() = delete; // uses only static methods
    JUCE_DECLARE_NON_COPYABLE (SystemStats)
};

} // namespace juce
