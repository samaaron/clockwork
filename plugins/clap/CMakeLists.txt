# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2025-2026 Sam Aaron
#
# The CLAP half of plugin hosting.
#
# Three things live here and nothing else does: the CLAP headers, the adapter
# that implements src/plugin_host.h against them (src/plugin_clap.cpp, added to
# clockwork library from here so the top-level file needs one line, not five),
# and one trivial in-tree plugin so hosting is testable without installing a
# third-party binary.
#
# CLAP is MIT and header-only. There is no library to build, nothing to link,
# and no licence question to answer — which is why CLOCKWORK_PLUGINS defaults
# ON: this costs a shallow clone and some include paths.

# Installed headers first, the pinned fetch second. A distro package build runs
# with no network, so the headers have to be findable on the machine: either a
# system copy (CLAP_INCLUDE_DIR, or clap/clap.h on the default include path)
# or one the packager unpacked and pointed at. Nothing in Debian ships CLAP
# today, so the variable is how a packager supplies it. Either way the target
# is the same `clap` interface the adapter and the test plugin link.
find_path(CLAP_INCLUDE_DIR clap/clap.h)
if(CLAP_INCLUDE_DIR)
    add_library(clap INTERFACE)
    target_include_directories(clap INTERFACE "${CLAP_INCLUDE_DIR}")
    message(STATUS "CLAP: installed headers at ${CLAP_INCLUDE_DIR}")
else()
    include(FetchContent)

    # 1.2.10, the current release. Pinned by tag, not by branch: a host's ABI
    # expectations are a version, not a moving target.
    FetchContent_Declare(clap
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG        1.2.10
        GIT_SHALLOW    TRUE
        ${_ss_fc_exclude})   # keep CLAP's install() rules out of our manifest
    set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(clap)

    if(NOT TARGET clap)
        message(FATAL_ERROR "FetchContent(clap) defined no `clap` target")
    endif()
    message(STATUS "CLAP: fetched 1.2.10 (no installed headers found)")
endif()

# ─── The adapter ─────────────────────────────────────────────────────────────
# src/plugin_clap.cpp, built here rather than into clockwork: plugin hosting
# is native-only and optional, and clockwork's core contract must not come to
# depend on it (docs/TRACKS.md, "the third real risk").
#
# It exports clockwork_plugin_clap::*, not plugin_host.h's C symbols — plugin_host.cpp
# is the one definition of those, because two adapters defining plugin_scan is a
# duplicate symbol at link.
add_library(clockwork_plugin_clap STATIC "${CLOCKWORK_SRC}/plugin_clap.cpp")
# clockwork-clock/cpp: as for the VST3 adapter — the transport event is filled from
# a clockwork_clock.h ClockworkTimeline (src/plugin_transport.h).
target_include_directories(clockwork_plugin_clap PUBLIC "${CLOCKWORK_SRC}" "${CLOCKWORK_RUST_DIR}/clockwork-clock/cpp")
# The CLAP headers are PRIVATE: nothing outside the adapter includes one, which
# is what keeps plugin_host.h format-blind. libdl is PUBLIC because dlopen is
# reached from this static library's own code and must resolve at final link —
# CMAKE_DL_LIBS is libdl on older glibc, empty where libc already has it.
target_link_libraries(clockwork_plugin_clap
    PRIVATE clap
    PUBLIC  ${CMAKE_DL_LIBS})
# PUBLIC: plugin_host.cpp compiles its CLAP branch only when this is set.
target_compile_definitions(clockwork_plugin_clap PUBLIC CLOCKWORK_PLUGIN_CLAP=1)
set_target_properties(clockwork_plugin_clap PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ─── The test plugin ─────────────────────────────────────────────────────────
# Behaviour is a spec shared with the VST3 test plugin — see the source. Built
# as a MODULE (a plain dlopen-able object, never linked against) into
# <build>/plugins so a test can load it by path.
add_library(clockwork_clap_gain MODULE clockwork_clap_gain.c)
target_link_libraries(clockwork_clap_gain PRIVATE clap)
set_target_properties(clockwork_clap_gain PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    PREFIX ""
    SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
if(NOT MSVC)
    target_compile_options(clockwork_clap_gain PRIVATE -Wall -Wextra)
endif()
