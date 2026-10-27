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

#ifdef JUCE_AUDIO_BASICS_H_INCLUDED
 /* When you add this cpp file to your project, you mustn't include it in a file where you've
    already included any other headers - just put it inside a file on its own, possibly with your config
    flags preceding it, but don't include anything else. That also includes avoiding any automatic prefix
    header files that the compiler may be using.
 */
 #error "Incorrect use of JUCE cpp file"
#endif

#include "juce_audio_basics.h"

#if JUCE_MINGW && ! defined (alloca)
 #define alloca __builtin_alloca
#endif

#if JUCE_USE_SSE_INTRINSICS
 #include <emmintrin.h>
#endif

#if JUCE_MAC || JUCE_IOS
 #ifndef JUCE_USE_VDSP_FRAMEWORK
  #define JUCE_USE_VDSP_FRAMEWORK 1
 #endif

 #if JUCE_USE_VDSP_FRAMEWORK
  #include <Accelerate/Accelerate.h>
 #endif

 #include "native/juce_AudioWorkgroup_mac.h"

#elif JUCE_USE_VDSP_FRAMEWORK
 #undef JUCE_USE_VDSP_FRAMEWORK
#endif

#if JUCE_USE_ARM_NEON
 #include <arm_neon.h>
#endif

#include "buffers/juce_FloatVectorOperations.cpp"
#include "buffers/juce_AudioChannelSet.cpp"
#include "utilities/juce_AudioWorkgroup.cpp"

