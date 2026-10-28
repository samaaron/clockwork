# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# Fetch Ableton Link at the pinned tag, apply clockwork's patches, and define
# the Ableton::Link interface target. Used by the main tree (CLOCKWORK_LINK)
# and by the fuzz build (test/fuzz), so the pin and the patches live once.
#
#   include(external/fetch-link.cmake)
#   clockwork_fetch_link()
#
# The caller's CMAKE_CXX_STANDARD survives: Link's own config file sets 17 in
# the including scope, and is undone here.

include(FetchContent)

function(clockwork_fetch_link)
    message(STATUS "Ableton Link: fetching Link-4.1.1 (GPL-2.0-or-later)")
    FetchContent_Declare(AbletonLink
        GIT_REPOSITORY https://github.com/Ableton/link.git
        GIT_TAG        Link-4.1.1
        GIT_SHALLOW    TRUE
        # Link patches, applied through cmake -P so no Unix shell is needed on
        # Windows. Two of them are not optional: LinkSession.cpp calls
        # loopbackOnly() and sessionPeers(), which Link-4.1.1 does not have —
        # this option could not build without them, which is why it had never
        # been built. See each patch header for what it does and why.
        PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DLINK_PATCH_DIR=${CMAKE_CURRENT_FUNCTION_LIST_DIR}
                       -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apply-link-patches.cmake)
    FetchContent_GetProperties(AbletonLink)
    if(NOT abletonlink_POPULATED)
        FetchContent_Populate(AbletonLink)
    endif()
    # AbletonLinkConfig.cmake hard-sets CMAKE_CXX_STANDARD 17 in the calling
    # scope — save/restore so C++20 survives.
    set(_saved_cxx_standard ${CMAKE_CXX_STANDARD})
    include(${abletonlink_SOURCE_DIR}/AbletonLinkConfig.cmake)
    set(CMAKE_CXX_STANDARD ${_saved_cxx_standard})
    # ConfigureAbletonLink declares `add_library(Ableton::Link IMPORTED
    # INTERFACE)`, and an imported target belongs to the directory that created
    # it — here. Anything outside this subdirectory that names it gets "the
    # target was not found", which is what the out-of-process Link peer under
    # test/native hit. Promote it so siblings can link it too.
    if(TARGET Ableton::Link)
        set_target_properties(Ableton::Link PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
    set(abletonlink_SOURCE_DIR ${abletonlink_SOURCE_DIR} PARENT_SCOPE)
endfunction()
