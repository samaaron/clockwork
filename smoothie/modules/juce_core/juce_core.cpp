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

#ifdef JUCE_CORE_H_INCLUDED
 /* When you add this cpp file to your project, you mustn't include it in a file where you've
    already included any other headers - just put it inside a file on its own, possibly with your config
    flags preceding it, but don't include anything else. That also includes avoiding any automatic prefix
    header files that the compiler may be using.
 */
 #error "Incorrect use of JUCE cpp file"
#endif

#define JUCE_CORE_INCLUDE_OBJC_HELPERS 1
#define JUCE_CORE_INCLUDE_COM_SMART_PTR 1
#define JUCE_CORE_INCLUDE_NATIVE_HEADERS 1
#define JUCE_CORE_INCLUDE_JNI_HELPERS 1

#include "juce_core.h"

#include <cctype>
#include <cstdarg>
#include <locale>
#include <thread>

#if ! (JUCE_ANDROID || JUCE_BSD)
 #include <sys/timeb.h>
 #include <cwctype>
#endif

#if JUCE_WINDOWS
 #include <ctime>

 #if JUCE_MINGW
  #include <ws2spi.h>
  #include <cstdio>
  #include <locale.h>
 #else
  JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4091)
  #include <Dbghelp.h>
  JUCE_END_IGNORE_WARNINGS_MSVC

  #if ! JUCE_DONT_AUTOLINK_TO_WIN32_LIBRARIES
   #pragma comment (lib, "DbgHelp.lib")
  #endif
 #endif

#else
 #if JUCE_LINUX || JUCE_BSD || JUCE_ANDROID
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/errno.h>
  #include <unistd.h>
  #include <netinet/in.h>
 #endif

 #if JUCE_WASM
  #include <stdio.h>
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <errno.h>
  #include <unistd.h>
  #include <netinet/in.h>
  #include <sys/stat.h>
 #endif

 #if JUCE_LINUX || JUCE_BSD
  #include <stdio.h>
  #include <langinfo.h>
  #include <ifaddrs.h>
  #include <sys/resource.h>

  #if JUCE_USE_CURL
   #include <curl/curl.h>
  #endif
 #endif

 #include <pwd.h>
 #include <fcntl.h>
 #include <netdb.h>
 #include <arpa/inet.h>
 #include <netinet/tcp.h>
 #include <sys/time.h>
 #include <net/if.h>
 #include <sys/ioctl.h>

 #if ! (JUCE_ANDROID || JUCE_WASM)
  #include <execinfo.h>
 #endif
#endif

#if JUCE_MAC || JUCE_IOS
 #include <xlocale.h>
 #include <mach/mach.h>
#endif

#if JUCE_ANDROID
 #include <ifaddrs.h>
 #include <android/log.h>
#endif

#undef check

//==============================================================================
#include "containers/juce_ArrayBase.cpp"
#include "containers/juce_ListenerList.cpp"
#include "containers/juce_OwnedArray.cpp"
#include "containers/juce_ReferenceCountedArray.cpp"
#include "logging/juce_Logger.cpp"
#include "memory/juce_MemoryBlock.cpp"
#include "misc/juce_RuntimePermissions.cpp"
#include "misc/juce_ScopeGuard.cpp"
#include "system/juce_SystemStats.cpp"
#include "threads/juce_Thread.cpp"
#include "time/juce_Time.cpp"
#include "native/juce_ThreadPriorities_native.h"

//==============================================================================
#if ! JUCE_WINDOWS
 #include "native/juce_SharedCode_posix.h"
 #if ! JUCE_ANDROID || __ANDROID_API__ >= 24
 #endif
#endif

//==============================================================================
#if JUCE_MAC || JUCE_IOS
 #include "native/juce_Strings_mac.mm"
 #include "native/juce_SharedCode_intel.h"
 #include "native/juce_SystemStats_mac.mm"
 #include "native/juce_Threads_mac.mm"

//==============================================================================
#elif JUCE_WINDOWS
 #include "native/juce_SystemStats_windows.cpp"
 #include "native/juce_Threads_windows.cpp"

//==============================================================================
#elif JUCE_LINUX
 #if JUCE_USE_CURL
 #endif
 #include "native/juce_SystemStats_linux.cpp"
 #include "native/juce_Threads_linux.cpp"

//==============================================================================
#elif JUCE_BSD
 #if JUCE_USE_CURL
 #endif
 #include "native/juce_SharedCode_intel.h"
 #include "native/juce_SystemStats_linux.cpp"
 #include "native/juce_Threads_linux.cpp"

//==============================================================================
#elif JUCE_ANDROID
 #include "native/juce_JNIHelpers_android.cpp"
 #include "native/juce_Misc_android.cpp"
 #include "native/juce_SystemStats_android.cpp"
 #include "native/juce_Threads_android.cpp"
 #include "native/juce_RuntimePermissions_android.cpp"

//==============================================================================
#elif JUCE_WASM
 #include "native/juce_SystemStats_wasm.cpp"
#endif

#include "threads/juce_WaitableEvent.cpp"

#if ! JUCE_WASM
#endif

//==============================================================================
//==============================================================================
namespace juce
{
/*
    As the very long class names here try to explain, the purpose of this code is to cause
    a linker error if not all of your compile units are consistent in the options that they
    enable before including JUCE headers. The reason this is important is that if you have
    two cpp files, and one includes the juce headers with debug enabled, and the other doesn't,
    then each will be generating code with different memory layouts for the classes, and
    you'll get subtle and hard-to-track-down memory corruption bugs!
*/
#if JUCE_DEBUG
 this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_debug_mode
    ::this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_debug_mode() noexcept {}
#else
 this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_release_mode
    ::this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_release_mode() noexcept {}
#endif
}
