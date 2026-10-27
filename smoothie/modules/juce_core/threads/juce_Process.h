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
/** The current process, as far as the device layer asks about it: whether a
    debugger is attached, and on Windows the module handle the hidden message
    window is registered against. JUCE's priority, privilege, foreground and
    dock-icon controls are gone; none of Smoothie's drivers used them.

    @tags{Core}
*/
class JUCE_API  Process
{
public:
    //==============================================================================
    /** Returns true if this process is being hosted by a debugger. */
    static bool JUCE_CALLTYPE isRunningUnderDebugger() noexcept;

    //==============================================================================
   #if JUCE_WINDOWS || DOXYGEN
    /** WINDOWS ONLY - Gets the HINSTANCE of the current module. */
    static void* JUCE_CALLTYPE getCurrentModuleInstanceHandle() noexcept;

    /** WINDOWS ONLY - Sets a new module handle to be used by the library. */
    static void JUCE_CALLTYPE setCurrentModuleInstanceHandle (void* newHandle) noexcept;
   #endif

private:
    Process();
    JUCE_DECLARE_NON_COPYABLE (Process)
};

} // namespace juce
