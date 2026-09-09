# SPDX-License-Identifier: ISC
# Copyright (c) 2026 Sam Aaron

# Smoothie — clockwork's vendored ISC audio-device layer (see README.md).
# One static library compiling each module's unity source, JUCE-module-format
# compatible: `modules/` is the exported include root, so existing
# `#include <juce_core/juce_core.h>` lines resolve unchanged.

add_library(smoothie STATIC)

set(_smoothy_modules juce_core juce_events juce_audio_basics juce_audio_devices)

foreach(m IN LISTS _smoothy_modules)
    # JUCE module rule: on Apple compile the .mm twin (Objective-C++) and skip
    # the .cpp; elsewhere compile the .cpp.
    if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/${m}/${m}.mm")
        target_sources(smoothie PRIVATE "modules/${m}/${m}.mm")
    else()
        target_sources(smoothie PRIVATE "modules/${m}/${m}.cpp")
    endif()
endforeach()

target_include_directories(smoothie PUBLIC modules)
target_compile_features(smoothie PUBLIC cxx_std_17)

target_compile_definitions(smoothie PUBLIC
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_STANDALONE_APPLICATION=1
    JUCE_USE_CURL=0
    JUCE_MODULE_AVAILABLE_juce_core=1
    JUCE_MODULE_AVAILABLE_juce_events=1
    JUCE_MODULE_AVAILABLE_juce_audio_basics=1
    JUCE_MODULE_AVAILABLE_juce_audio_devices=1
    $<$<CONFIG:Debug>:DEBUG=1 _DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>
)

# juce_core carries its own zlib and compiles it by default. A distro build
# must use the system one instead — an embedded zlib is a security-update
# problem the distro will not accept — so CLOCKWORK_SYSTEM_ZLIB turns the
# bundled code off (JUCE_INCLUDE_ZLIB_CODE=0 makes juce_core include <zlib.h>
# in its zlibNamespace) and links ZLIB::ZLIB. Off, the tree stays
# self-contained, which is what macOS and Windows builds rely on.
if(CLOCKWORK_SYSTEM_ZLIB)
    find_package(ZLIB REQUIRED)
    target_compile_definitions(smoothie PUBLIC JUCE_INCLUDE_ZLIB_CODE=0)
    target_link_libraries(smoothie PUBLIC ZLIB::ZLIB)
    message(STATUS "smoothie: system zlib ${ZLIB_VERSION_STRING} (JUCE_INCLUDE_ZLIB_CODE=0)")
endif()

if(APPLE)
    # Framework lists from the vendored modules' metadata blocks:
    # core: Cocoa Foundation IOKit Security | audio_basics: Accelerate |
    # audio_devices: CoreAudio CoreMIDI AudioToolbox
    target_link_libraries(smoothie PUBLIC
        "-framework Cocoa"
        "-framework Foundation"
        "-framework IOKit"
        "-framework Security"
        "-framework Accelerate"
        "-framework CoreAudio"
        "-framework CoreMIDI"
        "-framework AudioToolbox")
elseif(WIN32)
    # MSVC: the modules pull their platform libs via #pragma comment(lib,...).
    target_compile_definitions(smoothie PUBLIC _UNICODE UNICODE NOMINMAX)
elseif(UNIX)
    # core metadata: rt dl pthread | audio_devices metadata: alsa
    find_package(Threads REQUIRED)
    find_package(ALSA REQUIRED)
    target_link_libraries(smoothie PUBLIC
        Threads::Threads ALSA::ALSA ${CMAKE_DL_LIBS} rt)
    # JACK + PipeWire backends are dlopen'd at runtime — compile-time flag
    # only, no link dependency (see root CMakeLists for the PipeWire glue).
    target_compile_definitions(smoothie PUBLIC JUCE_JACK=1)
endif()
