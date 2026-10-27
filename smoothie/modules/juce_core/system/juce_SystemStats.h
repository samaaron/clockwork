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
    only what its drivers use: the JUCE version string, the Windows version
    (WASAPI's modes depend on it), the device manufacturer on Android, and
    whether the process is an app extension on Apple platforms.

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

    //==============================================================================
    /** The set of possible results of the getOperatingSystemType() method. */
    enum OperatingSystemType
    {
        UnknownOS       = 0,

        MacOSX          = 0x0100,
        Windows         = 0x0200,   /**< To test whether any version of Windows is running,
                                         you can use the expression ((getOperatingSystemType() & Windows) != 0). */
        Linux           = 0x0400,
        Android         = 0x0800,
        iOS             = 0x1000,
        WASM            = 0x2000,

        Win2000         = Windows | 1,
        WinXP           = Windows | 2,
        WinVista        = Windows | 3,
        Windows7        = Windows | 4,
        Windows8_0      = Windows | 5,
        Windows8_1      = Windows | 6,
        Windows10       = Windows | 7,
        Windows11       = Windows | 8
    };

    /** Returns the type of operating system we're running on. Implemented on
        Windows, the one platform whose drivers ask.
    */
    static OperatingSystemType getOperatingSystemType();

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
