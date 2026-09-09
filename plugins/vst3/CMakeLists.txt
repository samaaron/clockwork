# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2025-2026 Sam Aaron
#
# The VST3 half of plugin hosting: the interface headers, the adapter, and one
# trivial plugin to test the adapter against.
#
# THE TAG IS A LICENSING DECISION, not a version one. v3.8.1_build_84 is the
# FIRST vst3_pluginterfaces release carrying an MIT LICENSE.txt (© 2026
# Steinberg); v3.8.0 and everything before it is the old
# Steinberg-proprietary-OR-GPLv3 pair, under which a VST3-hosting build could
# not have claimed MIT at all. Do not move this pin backwards.
#
# INTERFACE HEADERS ONLY. Neither the SDK's base/ nor its public.sdk/ is
# fetched: both the adapter and the test plugin implement the COM ABI by hand.
# That is more code once and far less to keep in step afterwards.
#
# MIT covers the CODE. "VST" is a Steinberg trademark and its use in naming
# and badging is governed separately — that constrains how a feature is
# described, not whether it can be built.

# THREE LOADERS, one per platform, all in plugin_vst3.cpp. Linux loads a bare
# .so through ModuleEntry; macOS loads the executable inside the .vst3 bundle
# and initialises it through bundleEntry(CFBundleRef), which is why the adapter
# builds a CFBundle from the bundle root rather than passing null — plugins
# with resources beside their binary (Surge's wavetables, any editor's images)
# need it to find them. Windows loads the DLL inside the bundle (it keeps the
# .vst3 suffix) with LoadLibraryExW and initialises it through InitDll. The
# Windows branch is written against the specification and has not yet been
# compiled or run on Windows: see docs/TRACKS.md.

# Installed headers first, the pinned fetch second. A distro package build
# runs with no network, and Debian ships the SDK as libvst3sdk-dev with the
# headers under /usr/include/vst3sdk/pluginterfaces/ — hence the vst3sdk
# suffix. VST3_PLUGINTERFACES_INCLUDE_DIR names any other root that holds a
# pluginterfaces/ directory.
#
# An installed copy older than 3.8.1 is under the earlier proprietary-OR-GPLv3
# pair (Debian trixie's 3.7.10 is). That is fine for a copyleft build of
# clockwork and wrong for a commercial one, which is what the warning below
# is for; the pin above stays where the licensing note says it stays.
find_path(VST3_PLUGINTERFACES_INCLUDE_DIR pluginterfaces/base/funknown.h
    PATH_SUFFIXES vst3sdk)
if(VST3_PLUGINTERFACES_INCLUDE_DIR)
    set(_vst3_inc "${VST3_PLUGINTERFACES_INCLUDE_DIR}")
    set(_vst3_version "unknown")
    if(EXISTS "${_vst3_inc}/pluginterfaces/vst/vsttypes.h")
        file(STRINGS "${_vst3_inc}/pluginterfaces/vst/vsttypes.h" _vst3_version_line
             REGEX "define kVstVersionString")
        if(_vst3_version_line MATCHES "\"VST ([0-9]+\.[0-9]+(\.[0-9]+)?)\"")
            set(_vst3_version "${CMAKE_MATCH_1}")
        endif()
    endif()
    message(STATUS "VST3: installed pluginterfaces ${_vst3_version} at ${_vst3_inc}")
    if(_vst3_version STREQUAL "unknown" OR _vst3_version VERSION_LESS 3.8.1)
        message(WARNING "VST3 pluginterfaces ${_vst3_version} predates the MIT "
            "relicensing at 3.8.1: this build takes the SDK under GPLv3, which "
            "a commercially licensed clockwork cannot.")
    endif()
else()
    include(FetchContent)

    FetchContent_Declare(vst3_pluginterfaces
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
        GIT_TAG        v3.8.1_build_84
        GIT_SHALLOW    TRUE
        ${_ss_fc_exclude})
    FetchContent_GetProperties(vst3_pluginterfaces)
    if(NOT vst3_pluginterfaces_POPULATED)
        FetchContent_Populate(vst3_pluginterfaces)
    endif()

    # The headers include each other as "pluginterfaces/…", but the checkout's
    # root IS pluginterfaces — so it needs wrapping under a directory of that
    # name.
    #
    # The wrapper lives inside the checkout so the headers stay alongside the
    # LICENSE of the tree they were fetched from.
    set(_vst3_inc "${vst3_pluginterfaces_SOURCE_DIR}")
    file(MAKE_DIRECTORY "${_vst3_inc}/pluginterfaces")
    file(GLOB _vst3_roots RELATIVE "${_vst3_inc}" "${_vst3_inc}/*")
    foreach(_d IN LISTS _vst3_roots)
        if(IS_DIRECTORY "${_vst3_inc}/${_d}" AND NOT _d STREQUAL "pluginterfaces"
           AND NOT _d MATCHES "^\\.")
            file(CREATE_LINK "${_vst3_inc}/${_d}" "${_vst3_inc}/pluginterfaces/${_d}"
                 SYMBOLIC RESULT _vst3_link_result)
            # Windows refuses symlinks without Developer Mode or elevation, and
            # says so in RESULT rather than failing the configure — so the
            # wrapper would silently come out empty and the headers go missing
            # at compile time. Copy instead: they are headers, and the point is
            # only that "pluginterfaces/..." resolves.
            if(NOT _vst3_link_result STREQUAL "0")
                file(COPY "${_vst3_inc}/${_d}"
                     DESTINATION "${_vst3_inc}/pluginterfaces")
            endif()
        endif()
    endforeach()
    message(STATUS "VST3: fetched pluginterfaces 3.8.1 (no installed copy found)")
endif()

add_library(clockwork_vst3_pluginterfaces INTERFACE)
target_include_directories(clockwork_vst3_pluginterfaces INTERFACE "${_vst3_inc}")

# ─── The adapter ─────────────────────────────────────────────────────────────
# src/plugin_vst3.cpp, built here rather than into clockwork: plugin hosting
# is native-only and optional, and clockwork's core contract must not come
# to depend on it (docs/TRACKS.md, "the third real risk").
add_library(clockwork_plugin_vst3 STATIC
    "${CLOCKWORK_SRC}/plugin_vst3.cpp")
# clockwork-clock/cpp: the timeline snapshot the adapter fills ProcessContext from
# (src/plugin_transport.h) is clockwork_clock.h's ClockworkTimeline; its arithmetic links
# from whichever Rust umbrella the binary carries (clockwork-native, clockwork-bridge-native).
target_include_directories(clockwork_plugin_vst3 PUBLIC "${CLOCKWORK_SRC}" "${CLOCKWORK_RUST_DIR}/clockwork-clock/cpp")
target_link_libraries(clockwork_plugin_vst3
    PRIVATE clockwork_vst3_pluginterfaces
    PUBLIC  ${CMAKE_DL_LIBS})
if(APPLE)
    # CFBundleCreate / CFRelease: the loader hands each plugin its own bundle
    # so it can reach the resources beside its binary. See plugin_vst3.cpp.
    target_link_libraries(clockwork_plugin_vst3 PRIVATE "-framework CoreFoundation")
endif()
# PUBLIC: plugin_host.cpp compiles its VST3 branch only when this is set.
target_compile_definitions(clockwork_plugin_vst3 PUBLIC CLOCKWORK_PLUGIN_VST3=1)
set_target_properties(clockwork_plugin_vst3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ─── The test plugin ─────────────────────────────────────────────────────────
# Built into the tree in the VST3 bundle layout a real plugin has, so the
# adapter's bundle resolution is exercised by the tests rather than assumed.
add_library(ClockworkTestGain MODULE ClockworkTestGain.cpp)
target_link_libraries(ClockworkTestGain PRIVATE clockwork_vst3_pluginterfaces)
# The layout differs per platform and so does the file name: Linux wants
# Contents/<arch>-linux/Name.so, macOS wants Contents/MacOS/Name with no suffix
# at all, and Windows wants Contents/<arch>-win/Name.vst3 — a DLL that keeps
# the bundle's own suffix. Building it the way the platform really does is the
# point — it is what exercises resolveModulePath instead of asserting it.
if(APPLE)
    set(_clockwork_testgain_suffix "")
    set(_clockwork_testgain_dir
        "${CMAKE_BINARY_DIR}/plugins/ClockworkTestGain.vst3/Contents/MacOS")
elseif(WIN32)
    # The architecture folder is the specification's name, not CMake's:
    # CMAKE_SYSTEM_PROCESSOR says "AMD64" or "ARM64" here, where a bundle
    # says x86_64-win or arm64-win. (An arm64 host running an x64 toolchain
    # reports the toolchain's target, which is the one that matters.)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_clockwork_testgain_arch "arm64-win")
    else()
        set(_clockwork_testgain_arch "x86_64-win")
    endif()
    set(_clockwork_testgain_suffix ".vst3")
    # $<1:…>: a multi-config generator (Visual Studio) otherwise appends its
    # own Debug/ or Release/ to a plain output directory, and the module would
    # land one level too deep for resolveModulePath to see it.
    set(_clockwork_testgain_dir
        "$<1:${CMAKE_BINARY_DIR}/plugins/ClockworkTestGain.vst3/Contents/${_clockwork_testgain_arch}>")
else()
    set(_clockwork_testgain_suffix ".so")
    set(_clockwork_testgain_dir
        "${CMAKE_BINARY_DIR}/plugins/ClockworkTestGain.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux")
endif()
# Both output directories: CMake files a MODULE's Windows DLL under LIBRARY
# (unlike a SHARED library's, which is RUNTIME), and setting the pair means
# the bundle location does not hinge on remembering which rule applies.
set_target_properties(ClockworkTestGain PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME "ClockworkTestGain"
    PREFIX ""
    SUFFIX "${_clockwork_testgain_suffix}"
    LIBRARY_OUTPUT_DIRECTORY "${_clockwork_testgain_dir}"
    RUNTIME_OUTPUT_DIRECTORY "${_clockwork_testgain_dir}")

# Where the tests find it. Cached so test/CMakeLists.txt can read it whatever
# order the directories are added in.
set(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE
    "${CMAKE_BINARY_DIR}/plugins/ClockworkTestGain.vst3" CACHE INTERNAL "")

