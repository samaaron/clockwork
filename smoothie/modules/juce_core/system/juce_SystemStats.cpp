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

String SystemStats::getJUCEVersion()
{
    // Some basic tests, to keep an eye on things and make sure these types work ok
    // on all platforms. Let me know if any of these assertions fail on your system!
    static_assert (sizeof (pointer_sized_int) == sizeof (void*), "Basic sanity test failed: please report!");
    static_assert (sizeof (int8) == 1,                           "Basic sanity test failed: please report!");
    static_assert (sizeof (uint8) == 1,                          "Basic sanity test failed: please report!");
    static_assert (sizeof (int16) == 2,                          "Basic sanity test failed: please report!");
    static_assert (sizeof (uint16) == 2,                         "Basic sanity test failed: please report!");
    static_assert (sizeof (int32) == 4,                          "Basic sanity test failed: please report!");
    static_assert (sizeof (uint32) == 4,                         "Basic sanity test failed: please report!");
    static_assert (sizeof (int64) == 8,                          "Basic sanity test failed: please report!");
    static_assert (sizeof (uint64) == 8,                         "Basic sanity test failed: please report!");

    return "JUCE v" JUCE_STRINGIFY (JUCE_MAJOR_VERSION)
                "." JUCE_STRINGIFY (JUCE_MINOR_VERSION)
                "." JUCE_STRINGIFY (JUCE_BUILDNUMBER);
}

#if JUCE_MAC || JUCE_IOS
bool juce_mainBundleIsAppExtension();
#endif

bool SystemStats::isRunningInAppExtensionSandbox() noexcept
{
   #if JUCE_MAC || JUCE_IOS
    return juce_mainBundleIsAppExtension();
   #else
    return false;
   #endif
}

} // namespace juce
