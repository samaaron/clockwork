# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2025-2026 Sam Aaron
#
# clockwork — everything around a realtime audio engine, without the DSP.
#
# Derived from a synth engine's build, with everything on the DSP's side of the
# boundary removed: no synthesis, no graph, no definition formats, no ESP32.
#
# VST3 and CLAP appear here, but as a HOST — clockwork loads and runs other
# people's plugins (plugins/, src/plugin_*). Being a plugin is a different
# thing and is not here. What is left is clockwork — rings, lanes, scheduler tags,
# shared memory, ClockworkClock, the comms client library, the JUCE audio
# backend, MIDI, gamepad — plus one DSP, chosen by CLOCKWORK_DSP, that
# supplies the seven symbols src/dsp_api.h declares.
#
# See docs/BOUNDARY.md.
cmake_minimum_required(VERSION 3.24)

# ─── Version ─────────────────────────────────────────────────────────────────
# Single-sourced from src/clockwork_config.h so project() and the binary's own
# -v banner agree.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/clockwork_config.h" _ss_version_defs
     REGEX "^#define CLOCKWORK_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_ss_line IN LISTS _ss_version_defs)
    if(_ss_line MATCHES "CLOCKWORK_VERSION_(MAJOR|MINOR|PATCH) +([0-9]+)")
        set(_ss_ver_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    endif()
endforeach()
if(NOT DEFINED _ss_ver_MAJOR OR NOT DEFINED _ss_ver_MINOR OR NOT DEFINED _ss_ver_PATCH)
    message(FATAL_ERROR "Could not parse CLOCKWORK_VERSION_* from src/clockwork_config.h")
endif()

project(Clockwork
    VERSION ${_ss_ver_MAJOR}.${_ss_ver_MINOR}.${_ss_ver_PATCH}
    LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

# C++20: std::atomic::wait() / notify_all() are used throughout the workers.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Options ─────────────────────────────────────────────────────────────────
# What this binary calls itself: the banner, every log line, the name given to
# hosted plugins, the shared-memory segment, the headless device. An EMBEDDER
# sets this; a standalone clockwork build keeps its own name. See src/clockwork_product.h.
set(CLOCKWORK_PRODUCT_NAME "clockwork" CACHE STRING
    "Product name used in banners, logs and OS-visible names")

# The product's own header — its boot banner lives there, being art rather
# than a word. Empty keeps clockwork's own. See src/clockwork_product.h.
set(CLOCKWORK_PRODUCT_HEADER "" CACHE FILEPATH
    "Header defining CLOCKWORK_PRODUCT_BANNER for this product")

# Shown beside the product name in Windows' version resource — which is what
# UAC and the microphone/permission dialogs display. An embedder sets both.
set(CLOCKWORK_COMPANY_NAME "${CLOCKWORK_PRODUCT_NAME}" CACHE STRING
    "Company name in the Windows version resource")

# The version stamped into the Windows resource. Defaults to clockwork's own,
# which is right standalone and wrong when embedded — an executable should
# carry the PRODUCT's version, and clockwork's project() version is not it.
set(CLOCKWORK_PRODUCT_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Version stamped into OS-visible metadata")

option(BUILD_TESTS "Build the native Catch2 test suite" OFF)

# The scheduler host and clockwork's own test suite exist to prove clockwork
# works on its own. A project that EMBEDS clockwork wants the library and not
# those: its own host is the point, and building ours would give it a second
# main() and a second DSP. Defaults to on when clockwork is the top-level
# project and off when it is a subdirectory, which is the right answer in
# both cases without anyone setting anything.
option(CLOCKWORK_STANDALONE
    "Build the scheduler host and clockwork's own tests"
    ${PROJECT_IS_TOP_LEVEL})

# The DSP. A directory under dsp/ holding a CMakeLists.txt that defines a
# target of the same name, implementing dsp_api.h. The default `dummy` makes a
# noise and exists only so this repository can build and test itself with no
# real DSP present.
set(CLOCKWORK_DSP "dummy" CACHE STRING
    "Which DSP to link (implements src/dsp_api.h)")

# Where that DSP's CMakeLists.txt lives. Empty means dsp/<CLOCKWORK_DSP>
# inside this repository, which is what a standalone build wants.
#
# A CONSUMER SETS THIS. Clockwork is a substrate: the project embedding it
# owns the DSP, and its engine has no business living inside clockwork's
# checkout — that would make clockwork un-updatable without carrying somebody
# else's engine along with it. Point this at a directory anywhere and it is
# added as a subdirectory with its own binary dir.
set(CLOCKWORK_DSP_DIR "" CACHE PATH
    "Directory holding the DSP's CMakeLists.txt (empty = dsp/<CLOCKWORK_DSP> in-tree)")

# ── Rust subsystems ──────────────────────────────────────────────────────────
# ── The native audio device layer ────────────────────────────────────────────
#
# OFF builds clockwork WITHOUT smoothie/JUCE: no device enumeration, no device
# callback, no recovery, no recording. What is left is the substrate — the
# lanes ABI, the clock, the rings, the scheduler, ports, sinks, MIDI, gamepad,
# OSC and the DSP boundary — for a host that owns its own audio callback and
# never opens a device.
#
# That is every embedded target, the AudioWorklet, and a plugin, where the DAW
# owns the device and a second device layer is at best dead weight. The
# coupling is confined to src/native/, so this is a source-list decision rather
# than an #ifdef through the tree.
#
# It also removes clockwork's only JUCE dependency, and with it the ISC/AGPL
# question smoothie exists to answer.
option(CLOCKWORK_DEVICE  "Build the native audio device layer (smoothie/JUCE)" ON)

option(CLOCKWORK_MIDI    "Integrate the Rust/midir MIDI subsystem"        ON)
option(CLOCKWORK_GAMEPAD "Integrate the Rust/gilrs gamepad subsystem"     ON)
option(CLOCKWORK_OSC     "Integrate the Rust/std::net OSC subsystem"      ON)

# ── Who builds the Rust half ─────────────────────────────────────────────────
# ON: cargo builds the Rust subsystems into one umbrella staticlib here, and
# every binary this tree links gets it. OFF: the embedder is a RUST program
# (rust/clockwork-sys) that takes the subsystem crates as dependencies of its
# own — one std, rustc's own link line — and this tree builds the C++ alone.
# The targets that need the umbrella are refused below rather than left to
# fail at link.
option(CLOCKWORK_RUST "Build the Rust subsystems' umbrella staticlib with cargo (OFF: a Rust embedder supplies them as crates)" ON)

# ── The timed-event store ────────────────────────────────────────────────────
# OFF removes clockwork's timed queue entirely — the Rust crate is not
# compiled and no store, slot pool or data pool is carried. What is lost with
# it: /clockwork-sys/schedule, /clockwork-sys/sched/flush, timed MIDI clock out and timed
# OSC forwarding. Everything timed, in other words, except what a DSP schedules
# for itself; immediate messages are unaffected and a timed message still
# reaches the DSP at once, carrying its original timetag, for the DSP to decide
# what to do with.
#
# DEFAULT ON, and the default is not a matter of taste. The two real consumers
# sit on opposite sides of it: a guest scheduled by message uses the store, a self-scheduling guest
# does not (its VM parks programs rather than messages). The failures are
# asymmetric — such a guest
# build that forgets to turn this off carries a store it never fills, which is
# waste and nothing worse; a message-scheduled build that forgets to turn it on loses
# timed delivery. Waste is the better failure, so the default absorbs it. Do
# not "tidy" it to OFF.
#
# This is a compile-time switch and DspInfo::holds_schedule is a runtime one,
# and they are not redundant: see src/scheduler/engine_schedule.h, which states
# how the two compose.
option(CLOCKWORK_SCHEDULER "Build clockwork's timed-event store (/clockwork-sys/schedule, /clockwork-sys/sched/flush, timed MIDI out, timed OSC forwarding). OFF forwards every message immediately, carrying its timetag." ON)

# The CLIENT half of the verb surface (docs/SURFACE.md): /clockwork/midi/out/*,
# /clockwork/midi/clock/beat and /clockwork/osc/send — the verbs that exist
# because something outside clockwork drives it one event at a time. A guest
# that is in charge of itself reaches a wire through clockwork_sink_send and
# never sends them, so a build shaped for such a guest turns this OFF and the
# verbs are not compiled: a client that sends one is refused by name, with
# the reason. The transport half — ports, devices, the clock, subscriptions —
# stays in every build, because owning the hardware is what clockwork is for.
# The timed store is the same split, already made: CLOCKWORK_SCHEDULER.
option(CLOCKWORK_CLIENT_VERBS "Answer the client half of the verb surface (/clockwork/midi/out/*, /clockwork/midi/clock/beat, /clockwork/osc/send). OFF refuses them by name; see docs/SURFACE.md" ON)

# ── Ableton Link: OFF by default, and buildable when you turn it on ──────────
#
# Link is GPL-2.0-or-later, and so is the receive-side renderer that carries
# its audio (src/native/vendor/LinkAudioInputRenderer.hpp, adapted from
# Ableton's own example and vendored here under its original notice). Taken
# under GPLv3, both combine with an AGPL binary. A build under the commercial
# licence cannot take them that way and needs Ableton's own terms. See LICENSE.
option(CLOCKWORK_LINK "Integrate Ableton Link, tempo and audio (GPL-2.0-or-later; fetches the library)" OFF)
option(CLOCKWORK_LINK_AUDIO "Link peer audio in and publish out (needs CLOCKWORK_LINK)" OFF)

# ─── BEAM NIF: clockwork as an Erlang/Elixir native library ────────────────
#
# src/nif/ is one of the reference hosts lanes.h names — a host that drives the
# engine through the ABI and contains no DSP knowledge, which is how "the boundary
# is host-agnostic" gets to be a checkable claim rather than an assertion. It
# came across with the extraction and its BUILD RULE DID NOT, so it has not
# been compiled here since: the source sat next to a CMakeLists that had never
# heard of it.
#
# The OTP headers are vendored (external/erlang_headers, Apache-2.0, (c)
# Ericsson) so building the NIF needs no Erlang installation — only running its
# tests does. erl_int_sizes_config.h is generated for the target because the
# sizes it records are the compiler's, not the header author's.
option(CLOCKWORK_NIF "Build the BEAM NIF shared library" OFF)

# Windows-only; a no-op elsewhere. The Steinberg ASIO SDK is dual-licensed,
# Steinberg-proprietary OR GPLv3. external/ASIOSDK2.3.4 is the SDK's source,
# taken under GPLv3, without the logo artwork and PDFs that no compiler opens.
# A build under the commercial licence cannot take it that way and needs
# Steinberg's own agreement. See LICENSE.
option(CLOCKWORK_ASIO "Compile JUCE's ASIO backend on Windows (Steinberg SDK, taken under GPLv3)" OFF)

# ── Audio files ──────────────────────────────────────────────────────────────
# There is no option here, and that is the point. Reading and writing audio
# files is src/clockwork_audio_file.h, compiled into clockwork from vendored
# single-file decoders and this repository's own FLAC encoder. No package to
# find, nothing fetched at configure time, and the same formats on every
# platform including WebAssembly — what a build reads is a property of the
# source rather than of the machine it was configured on.
#
# This replaced libsndfile, which was LGPL and dragged ogg, vorbis, FLAC and
# opus behind it as four more sub-projects. dr_wav, dr_flac and dr_mp3 are
# Unlicense or MIT-0 and stb_vorbis is public domain or MIT, so the whole
# path is permissive and a commercial embedder links clockwork and ships.
option(CLOCKWORK_CARGO_OFFLINE  "Build the Rust subsystems with --offline (requires pre-vendored crates)" OFF)
# Smoothie compiles the zlib inside juce_core unless told otherwise. A distro
# build must link the system zlib instead (Debian refuses embedded copies of
# it): this sets JUCE_INCLUDE_ZLIB_CODE=0 and links ZLIB::ZLIB. See
# smoothie/CMakeLists.txt.
option(CLOCKWORK_SYSTEM_ZLIB    "Link the system zlib instead of the copy inside smoothie" OFF)
# stb_vorbis is vendored unmodified (src/vendor/stb, v1.22). A distro build can
# take the packaged one instead — Debian's libstb-dev ships the declarations as
# <stb/stb_vorbis.h> and the implementation in libstb, with a pkg-config file —
# so the copy here is not compiled and the system library is linked. The other
# codecs (dr_wav, dr_flac, dr_mp3) have no distro package and stay vendored.
option(CLOCKWORK_SYSTEM_STB     "Use the system stb (pkg-config: stb) for stb_vorbis instead of the vendored copy" OFF)

# ── Plugin hosting ───────────────────────────────────────────────────────────
# Clockwork as a HOST of plugins rather than as one — see src/plugin_host.h
# and docs/TRACKS.md. One directory under plugins/ per format, each
# holding its adapter's build and a trivial in-tree test plugin.
#
# Defaults ON, unlike every other fetching option here, because it costs
# nothing to say yes to: CLAP is MIT and header-only, so there is no library to
# build and no licence question to answer. Native only by construction — WASM
# cannot load a native plugin, which is why hosting can never be part of
# clockwork's core contract.
#
# Each format looks for installed SDK headers first (CLAP_INCLUDE_DIR,
# VST3_PLUGINTERFACES_INCLUDE_DIR, or the usual include paths) and fetches the
# pinned copy only when there are none, so a distro build with no network can
# still say yes.
option(CLOCKWORK_PLUGINS "Host CLAP and VST3 plugins (system SDK headers if installed, else fetched)" ON)

# What the plugin-bridge process is called on disk and in the process list.
# Plugins run in this separate process (plugin_bridge.h), so it is the name a
# user sees when one misbehaves; an embedder gives it a product-facing one.
# The engine finds it by this name beside its own executable.
set(CLOCKWORK_PLUGIN_BRIDGE_NAME "clockwork-plugin-bridge" CACHE STRING
    "File name of the plugin-bridge executable, built beside the host binary")
# Where an INSTALLED engine finds the bridge when it is not beside the
# executable. The engine looks for CLOCKWORK_PLUGIN_BRIDGE_NAME as a sibling
# of its own binary first (that is how a build tree and an app bundle are laid
# out), then in this directory. A distribution that keeps helper programs out
# of the user's PATH installs the bridge here — Debian's /usr/libexec/<pkg>,
# say — and sets this to match. Empty means "beside the executable only".
set(CLOCKWORK_PLUGIN_BRIDGE_DIR "" CACHE PATH
    "Directory the engine also searches for the plugin bridge (an installed layout)")

# The bridge's identity to macOS. On macOS the bridge is built as an app
# bundle, <name>.app, because a bare executable has no bundle identifier and
# anything that picks an application by one — OBS's application capture, a
# ScreenCaptureKit filter, TCC — then cannot pick the bridge at all: its
# editor windows were listed by OBS but unselectable. (An Info.plist embedded
# in the executable's __info_plist section is NOT enough: LaunchServices
# reports a bundle identifier only for a bundle on disk; tried.) An embedder
# sets this beside the name.
set(CLOCKWORK_PLUGIN_BRIDGE_BUNDLE_ID "org.clockwork.plugin-bridge" CACHE STRING
    "Bundle identifier of the plugin-bridge app bundle (macOS)")

# ─── Release debug symbols ───────────────────────────────────────────────────
if(MSVC)
    add_compile_options("$<$<CONFIG:Release>:/Z7>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG;/OPT:REF;/OPT:ICF>")
else()
    add_compile_options("$<$<CONFIG:Release>:-g>")
endif()

# ─── IEEE single precision where float evaluates wider ───────────────────────
# 32-bit x86 defaults to -mfpmath=387, so float expressions evaluate in 80-bit
# x87 registers and round to 32 bits only when the compiler happens to spill —
# making the same arithmetic give different answers at different block sizes.
# The target is identified by FLT_EVAL_METHOD, the standard's own signal, not
# by processor name (the 32-bit build runs in a linux/386 container on an
# x86_64 kernel, so CMAKE_SYSTEM_PROCESSOR reads x86_64 while cc is i686).
if(NOT MSVC)
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
#include <cfloat>
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD == 0
#error float expressions already evaluate as float
#endif
int main() { return 0; }
" CLOCKWORK_FLOAT_EVALUATES_WIDER)
    if(CLOCKWORK_FLOAT_EVALUATES_WIDER)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-fexcess-precision=standard" CLOCKWORK_HAS_EXCESS_PRECISION)
        if(CLOCKWORK_HAS_EXCESS_PRECISION)
            set(CLOCKWORK_FLOAT_FLAGS -fexcess-precision=standard)
        else()
            set(CLOCKWORK_FLOAT_FLAGS -ffloat-store)
        endif()
        # For this tree, here; for whoever links clockwork, as a PUBLIC option
        # on the library targets below. An embedder's host and tests make the
        # same exactness claims against the same arithmetic, and
        # add_compile_options stops at this directory — an embedder's i386
        # build compiled its own targets wide and failed on values that print
        # identically on both sides of the ==.
        add_compile_options(${CLOCKWORK_FLOAT_FLAGS})
    endif()
endif()

include(FetchContent)
# FetchContent deps are private build details: keep them out of the install
# manifest. The keyword reached FetchContent_Declare in CMake 3.28.
set(_ss_fc_exclude "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(_ss_fc_exclude EXCLUDE_FROM_ALL)
endif()

# ─── Position-independent code, for the NIF ──────────────────────────────────
# A NIF is a shared object the BEAM dlopens, and on Linux a shared object
# cannot swallow a static archive built without -fPIC. The audio path makes it
# strict rather than advisable: `t_on_audio_thread` is a thread-local and its
# TLS relocation (R_X86_64_TPOFF32) has no non-PIC form in a .so at all, so
# the link fails outright instead of degrading.
#
# HERE, not beside the NIF target below, and that is the whole point: this
# variable initialises a target's POSITION_INDEPENDENT_CODE as the target is
# CREATED, so setting it after add_library(clockwork) reaches nothing already
# defined. It read as correct beside the NIF and did nothing. macOS's
# relocation model is indifferent, so only a Linux nif build ever said so.
if(CLOCKWORK_NIF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ─── Smoothie: the audio device layer ───────────────────────────────────
# The vendored ISC fork of the four permissive JUCE 7.0.12 modules. JUCE >= 8
# is AGPL; vendoring also removes any way for a distro build to bind a system
# JUCE 8+ by accident. See smoothie/README.md.
# ── What the device layer is required for ────────────────────────────────────
# Refused rather than worked around: each of these IS a device host, so a build
# that asks for one without a device layer is asking for something that cannot
# link. Saying so here beats an undefined symbol several minutes later.
if(NOT CLOCKWORK_DEVICE)
    foreach(_needs_device CLOCKWORK_NIF CLOCKWORK_PLUGINS)
        if(${_needs_device})
            message(FATAL_ERROR
                "${_needs_device}=ON needs CLOCKWORK_DEVICE=ON: it drives an audio "
                "device (or, for CLOCKWORK_PLUGINS, the track lanes that feed one). "
                "Build the substrate with -D${_needs_device}=OFF, or keep the "
                "device layer.")
        endif()
    endforeach()
endif()

if(CLOCKWORK_DEVICE)
    add_subdirectory(smoothie)
endif()

# ─── Ableton Link (opt-in, GPL) ──────────────────────────────────────────────
if(CLOCKWORK_LINK)
    message(STATUS "Ableton Link: fetching Link-4.0 (GPL-2.0-or-later)")
    FetchContent_Declare(AbletonLink
        GIT_REPOSITORY https://github.com/Ableton/link.git
        GIT_TAG        Link-4.0
        GIT_SHALLOW    TRUE
        # Link patches, applied through cmake -P so no Unix shell is needed on
        # Windows. Two of them are not optional: LinkSession.cpp calls
        # loopbackOnly() and sessionPeers(), which Link-4.0 does not have —
        # this option could not build without them, which is why it had never
        # been built. See each patch header for what it does and why.
        PATCH_COMMAND  ${CMAKE_COMMAND}
                       -DLINK_PATCH_DIR=${CMAKE_CURRENT_LIST_DIR}/external
                       -P ${CMAKE_CURRENT_LIST_DIR}/external/apply-link-patches.cmake)
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
else()
    message(STATUS "Ableton Link disabled — ClockworkClock uses its local-state backing")
endif()

# ─── Paths ───────────────────────────────────────────────────────────────────
# CURRENT, not CMAKE_SOURCE_DIR. When clockwork is embedded as a
# subdirectory, CMAKE_SOURCE_DIR is the EMBEDDER's root and every path built
# from it points into the wrong repository — which is how the plugin
# subdirectories failed the first time an embedder tried to build.
set(CLOCKWORK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(NATIVE_SRC  "${CMAKE_CURRENT_SOURCE_DIR}/src/native")
set(WORKERS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/workers")
set(CLOCKWORK_RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust")

# ─── Include directories + compile definitions ───────────────────────────────
#
# Both lists are CACHE INTERNAL because they are part of clockwork's contract with an
# embedder — see "The compile profile" below — and a consumer may be a sibling
# directory rather than a child, which PARENT_SCOPE does not reach.
set(CLOCKWORK_INCLUDE_DIRS
    ${NATIVE_SRC}/NativeShim   # first: shims emscripten/ on native builds
    ${NATIVE_SRC}
    ${WORKERS_SRC}
    ${CLOCKWORK_SRC}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CLOCKWORK_SRC}/vendor/oscpack
    CACHE INTERNAL "Include directories clockwork's sources need")

# Event-sink size classes, "<cell bytes>:<cells>,..." per kind. Empty means
# memory_profile.h's default for the selected device profile.
set(CLOCKWORK_OSC_SINK_CLASSES "" CACHE STRING
    "Outbound OSC sink classes, \"<cell bytes>:<cells>,...\" (empty = memory_profile.h)")
set(CLOCKWORK_MIDI_SINK_CLASSES "" CACHE STRING
    "MIDI sink classes, \"<cell bytes>:<cells>,...\" (empty = memory_profile.h)")
set(CLOCKWORK_SINK_CLASS_DEFS "")
if(CLOCKWORK_OSC_SINK_CLASSES)
    list(APPEND CLOCKWORK_SINK_CLASS_DEFS "CLOCKWORK_OSC_SINK_CLASSES=\"${CLOCKWORK_OSC_SINK_CLASSES}\"")
endif()
if(CLOCKWORK_MIDI_SINK_CLASSES)
    list(APPEND CLOCKWORK_SINK_CLASS_DEFS "CLOCKWORK_MIDI_SINK_CLASSES=\"${CLOCKWORK_MIDI_SINK_CLASSES}\"")
endif()

set(CLOCKWORK_COMPILE_DEFS
    CLOCKWORK_PRODUCT_NAME="${CLOCKWORK_PRODUCT_NAME}"
    $<$<BOOL:${CLOCKWORK_PRODUCT_HEADER}>:CLOCKWORK_PRODUCT_HEADER="${CLOCKWORK_PRODUCT_HEADER}">
    NDEBUG=1
    NOMINMAX=1
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN=1>
    # The Windows version clockwork targets. Stated, because otherwise a
    # dependency states it: asio — vendored inside Ableton Link, and reached
    # from HeadlessDriver.cpp once CLOCKWORK_LINK_AUDIO is on — defines
    # _WIN32_WINNT as Windows 7 when nothing else has, and prints a message
    # asking to be told. Windows 7 hides the high-resolution waitable timer
    # that driver asks for, so the build failed in exactly the configuration
    # docs/BUILDING.md says to test.
    $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>

    # The audio path is compiled in. This flag has never meant "the DSP itself":
    # it gates clockwork's engine lifecycle, per-block render and audio route
    # — all of which clockwork owns and drives through dsp_api.h. What
    # actually makes sound behind that call is the DSP's, and is not in this
    # repository either way.
    CLOCKWORK_SYNTH=1

    # Whether the native device layer is present. Published because a consumer
    # has to know: without it there is no ClockworkEngine to construct and no
    # device to open, and the host owns its own audio callback.
    CLOCKWORK_DEVICE=$<IF:$<BOOL:${CLOCKWORK_DEVICE}>,1,0>

    # Clockwork's timed queue: present, or not built at all. Every site that
    # parks, fires, flushes or counts a scheduled event is behind this.
    CLOCKWORK_SCHEDULER=$<IF:$<BOOL:${CLOCKWORK_SCHEDULER}>,1,0>

    # The client half of the verb surface: answered, or refused by name. The
    # list itself is clockwork_sys_is_client_verb() in src/clockwork_sys.h.
    CLOCKWORK_CLIENT_VERBS=$<IF:$<BOOL:${CLOCKWORK_CLIENT_VERBS}>,1,0>

    SCHEDULER_DATA_POOL_SIZE=524288
    SCHEDULER_SLOT_COUNT=512

    # The sink size classes, when a build overrides memory_profile.h's.
    ${CLOCKWORK_SINK_CLASS_DEFS}

    # Per-slot shm_audio_buffer ring duration, in seconds. Every TU that
    # includes shm_audio_buffer.hpp must agree, hence a global rather than a
    # per-target definition; test builds carry more so capture assertions have
    # samples to work with.
    CLOCKWORK_SHM_AUDIO_SECONDS=$<IF:$<BOOL:${BUILD_TESTS}>,10,1>
    CACHE INTERNAL "Compile definitions a header-only consumer must match")

# ─── Host sources ────────────────────────────────────────────────────────────
# The core: the ingress/egress lanes, the block driver, the heap, the scope
# planes and the region arithmetic.
# CACHE INTERNAL, like the include directories and compile definitions above:
# a freestanding or embedded target compiles clockwork with its OWN defines — no
# device layer, no JUCE, a different memory profile — so a prebuilt libtau.a is
# no use to it and it needs the sources, and it is a parent project, which a
# plain variable set here never reaches.
set(CLOCKWORK_CORE_SOURCES
    ${CLOCKWORK_SRC}/audio_processor.cpp
    # The client boundary (src/clockwork_client.h). In the core library rather
    # than beside a consumer: the engine's own transports call it to deliver a
    # remote peer's traffic, so it is not optional and there is no second copy.
    ${CLOCKWORK_SRC}/clockwork_client.cpp
    ${CLOCKWORK_SRC}/clockwork_asset_pool.cpp
    # An engine in your process (src/clockwork_embed.h): attach and render
    # here; the device boot is in the device layer below.
    ${CLOCKWORK_SRC}/embed/clockwork_embed.cpp
    ${CLOCKWORK_SRC}/lanes/lanes.cpp
    ${CLOCKWORK_SRC}/clockwork_heap.cpp
    ${CLOCKWORK_SRC}/scope_streams.cpp
    ${CLOCKWORK_SRC}/mem_region.cpp
    ${CLOCKWORK_SRC}/engine_support.cpp
    # MIDI clock out is ClockworkClock's: 24 ticks a beat, each carrying its own
    # time to its sink. Nothing to do with the timed queue, so not gated on it.
    ${CLOCKWORK_SRC}/clock/MidiClockOut.cpp
    CACHE INTERNAL "Clockwork's device-independent sources, for freestanding/embedded builds")

# The audio backend and its control surfaces.
# What a host needs whether or not it owns a device: the egress dispatcher and
# the ring reader. Neither mentions JUCE or ClockworkEngine.
set(CLOCKWORK_NATIVE_SOURCES
    ${NATIVE_SRC}/OscEgress.cpp
    ${WORKERS_SRC}/RingReader.cpp)

# The device layer: everything that opens, drives, watches, recovers or records
# an audio device. All of it needs smoothie, and none of it means anything to a
# host that was handed a callback. See CLOCKWORK_DEVICE.
if(CLOCKWORK_DEVICE)
    list(APPEND CLOCKWORK_NATIVE_SOURCES
        ${NATIVE_SRC}/HeadlessDriver.cpp
        ${NATIVE_SRC}/JuceAudioCallback.cpp
        ${NATIVE_SRC}/RealtimeThread.cpp
        ${NATIVE_SRC}/EngineControl.cpp
        ${NATIVE_SRC}/ClockworkEngine.cpp
        ${NATIVE_SRC}/ClockworkEmbedDevice.cpp
        ${NATIVE_SRC}/DevicePolicy.cpp
        ${NATIVE_SRC}/AsioDriverCheck.cpp
        ${NATIVE_SRC}/AudioRecovery.cpp
        ${NATIVE_SRC}/AggregateDeviceHelper.cpp)
endif()
if(APPLE)
    list(APPEND CLOCKWORK_NATIVE_SOURCES ${NATIVE_SRC}/MicPermission.mm)
    # Dispatches AppKit input to any window this process owns. Belongs to
    # clockwork rather than to the plugin library: it is about this process
    # having windows at all, not about what is drawn in them.
    list(APPEND CLOCKWORK_NATIVE_SOURCES ${NATIVE_SRC}/CocoaEventPump.mm)
endif()

# ClockworkClock is the one composition root on every native cell: the audio-thread
# time source (WallClock + IIR) through TimeSource, the MIDI follower-timeline
# registry through MidiTimelines, and — only when Link is on — the Link session.
# The audio half of Link is the engine's LinkAudioHost beside the clock, over
# the Link-Audio bus bridge. With Link off, LinkSession.h supplies the
# session-of-one and LinkAudioBridge.h the no-op bridge, both header-inline, so
# those two TUs are dropped rather than compiled empty.
set(CLOCKWORK_CLOCK_SOURCES
    ${CLOCKWORK_SRC}/clock/ClockworkClock.cpp
    ${CLOCKWORK_SRC}/clock/EngineClock.cpp
    ${CLOCKWORK_SRC}/clock/ClockworkClockNative.cpp
    ${CLOCKWORK_SRC}/clock/TimeSource.cpp
    ${CLOCKWORK_SRC}/clock/MidiTimelines.cpp
    ${NATIVE_SRC}/LinkAudioHost.cpp)
if(CLOCKWORK_LINK)
    list(APPEND CLOCKWORK_CLOCK_SOURCES
        ${CLOCKWORK_SRC}/clock/LinkSession.cpp
        ${NATIVE_SRC}/LinkAudioBridge.cpp)
endif()

# Audio file reading and writing (src/clockwork_audio_file.h). The decoders are
# vendored single-file libraries; flac_encoder.cpp is clockwork's own, because
# the permissive single-file codecs decode and do not encode.
set(CLOCKWORK_AUDIO_FILE_SOURCES
    ${CLOCKWORK_SRC}/clockwork_audio_file.cpp
    ${CLOCKWORK_SRC}/flac_encoder.cpp)
if(CLOCKWORK_SYSTEM_STB)
    # The system stb: declarations from <stb/stb_vorbis.h>, the decoder from
    # libstb. clockwork_audio_file.cpp switches its include on the define.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(STB REQUIRED IMPORTED_TARGET stb)
    message(STATUS "clockwork: system stb ${STB_VERSION} for stb_vorbis (CLOCKWORK_SYSTEM_STB)")
else()
    # Compiled as its own translation unit: consumers include it with
    # STB_VORBIS_HEADER_ONLY and get the declarations alone.
    list(APPEND CLOCKWORK_AUDIO_FILE_SOURCES ${CLOCKWORK_SRC}/vendor/stb/stb_vorbis.c)
endif()

# oscpack (MIT, Ross Bencina) — src/vendor/oscpack.
set(OSCPACK_SOURCES
    ${CLOCKWORK_SRC}/vendor/oscpack/osc/OscTypes.cpp
    ${CLOCKWORK_SRC}/vendor/oscpack/osc/OscOutboundPacketStream.cpp
    ${CLOCKWORK_SRC}/vendor/oscpack/osc/OscReceivedElements.cpp)

add_library(clockwork STATIC
    ${CLOCKWORK_CORE_SOURCES}
    ${CLOCKWORK_NATIVE_SOURCES}
    ${CLOCKWORK_CLOCK_SOURCES}
    ${CLOCKWORK_AUDIO_FILE_SOURCES}
    ${OSCPACK_SOURCES})

target_include_directories(clockwork PUBLIC ${CLOCKWORK_INCLUDE_DIRS})
if(CLOCKWORK_FLOAT_FLAGS)
    target_compile_options(clockwork PUBLIC ${CLOCKWORK_FLOAT_FLAGS})
endif()
if(CLOCKWORK_SYSTEM_STB)
    target_compile_definitions(clockwork PRIVATE CLOCKWORK_SYSTEM_STB=1)
    target_link_libraries(clockwork PUBLIC PkgConfig::STB)
endif()
target_compile_definitions(clockwork PUBLIC ${CLOCKWORK_COMPILE_DEFS})

# ─── The compile profile, as part of clockwork's interface ─────────────────────────
#
# Anything that addresses shm regions by compile-time constant must be built
# with the SAME profile as clockwork, or server_shm's layout guard rejects it at
# attach. Linking clockwork carries that automatically — but a consumer that is
# header-only BY DESIGN (an out-of-process peer, a layout dumper) never links
# it and cannot inherit PUBLIC defs. That is why CLOCKWORK_COMPILE_DEFS above is a
# CACHE INTERNAL variable and not a local list: it is part of clockwork's contract,
# beside CLOCKWORK_DSP and CLOCKWORK_RUST_WORKSPACE, and a consumer may be a sibling
# directory rather than a child, which PARENT_SCOPE does not reach.
#
# EXPORTED BECAUSE THE ALTERNATIVE FAILED SILENTLY. An embedder's shm peer read
# a variable that was set nowhere — left behind when the extraction renamed
# the upstream variable and moved it in here. CMake expands an
# unset variable to nothing without complaint, so the peer was built with no
# profile at all and every layout constant quietly took its default. A consumer
# should not have to guess this list, and should not be able to miss it by
# typo.

# C++20 as a USAGE REQUIREMENT, not just a directory variable. CMAKE_CXX_STANDARD
# above applies to this directory and stops at the submodule boundary, so an
# embedder's own targets compiled as C++17 and failed on things clockwork's
# headers and an embedder's tests both use — std::atomic::notify_all was the one
# that surfaced it. Anything linking clockwork now gets the standard with it.
target_compile_features(clockwork PUBLIC cxx_std_20)

# The audio path reads and writes through memory buffers; the file entry points
# open their own handles. stb_vorbis's stdio wrapper is unused either way, so
# it is compiled out rather than left as an unreferenced dependency on a file
# system that a WebAssembly build does not have.
target_compile_definitions(clockwork PRIVATE STB_VORBIS_NO_STDIO)
if(CLOCKWORK_DEVICE)
    target_link_libraries(clockwork PUBLIC smoothie)
endif()

# Native PipeWire backend on Linux. libpipewire-0.3 is dlopen'd at runtime, so
# only its headers are needed and only at build time; without them the backend
# is simply not compiled.
if(UNIX AND NOT APPLE AND CLOCKWORK_DEVICE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PIPEWIRE libpipewire-0.3)
    endif()
    if(PIPEWIRE_FOUND)
        target_sources(clockwork PRIVATE ${NATIVE_SRC}/PipeWireAudio.cpp)
        target_compile_definitions(clockwork PUBLIC CLOCKWORK_PIPEWIRE=1)
        target_include_directories(clockwork PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        message(STATUS "PipeWire backend: enabled (libpipewire-0.3 ${PIPEWIRE_VERSION})")
    else()
        message(STATUS "PipeWire backend: disabled (libpipewire-0.3 headers not found)")
    endif()
endif()

if(WIN32 AND CLOCKWORK_ASIO)
    # JUCE's juce_ASIO_windows.cpp includes the SDK headers when JUCE_ASIO=1.
    # ASIOSDK_DIR points at another copy's common/ directory; default is the
    # vendored one.
    if(NOT ASIOSDK_DIR)
        set(ASIOSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ASIOSDK2.3.4/common")
    endif()
    target_compile_definitions(smoothie PUBLIC JUCE_ASIO=1)
    target_include_directories(smoothie PUBLIC "${ASIOSDK_DIR}")
    message(STATUS "ASIO enabled (JUCE_ASIO=1, Steinberg SDK under GPLv3, ${ASIOSDK_DIR})")
endif()

if(CLOCKWORK_NIF)
    # (CMAKE_POSITION_INDEPENDENT_CODE is set far above, before the first
    # add_library — see there for why it cannot live here.)

    set(_clockwork_erl_headers ${CMAKE_CURRENT_SOURCE_DIR}/external/erlang_headers)
    include(CheckTypeSize)
    check_type_size("char"      SIZEOF_CHAR      BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("short"     SIZEOF_SHORT     BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("int"       SIZEOF_INT       BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("long"      SIZEOF_LONG      BUILTIN_TYPES_ONLY LANGUAGE C)
    check_type_size("long long" SIZEOF_LONG_LONG BUILTIN_TYPES_ONLY LANGUAGE C)
    configure_file(
        ${_clockwork_erl_headers}/erl_int_sizes_config.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/erlang_generated/erl_int_sizes_config.h)

    add_library(clockwork_nif SHARED ${NATIVE_SRC}/../nif/clockwork_nif.cpp)
    set_target_properties(clockwork_nif PROPERTIES
        PREFIX ""
        # clockwork.erl resolves the library by this name; see load_nif there.
        OUTPUT_NAME "clockwork"
        # ...but ONLY the runtime artefact. On Windows a SHARED target also
        # emits an import library, and OUTPUT_NAME would name that clockwork.lib
        # too — the exact path the static `clockwork` target already writes.
        # link.exe refuses that outright: "LNK1149: output filename matches
        # input filename". The .dll does not collide (it is redirected to the
        # build root below); the .lib nobody redirected did. Unix has no import
        # library, so this is inert there.
        ARCHIVE_OUTPUT_NAME "clockwork_nif"
        # The build root on every platform and configuration, not Release/ or
        # Debug/, so erlang:load_nif finds it at one path.
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}")
    target_include_directories(clockwork_nif PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/erlang_generated
        ${_clockwork_erl_headers})
    target_link_libraries(clockwork_nif PRIVATE
        clockwork_comms
        clockwork_bundle)

    if(APPLE)
        # load_nif wants .so on every Unix, not .dylib; and the BEAM's own
        # symbols are resolved at dlopen time, not at link time.
        set_target_properties(clockwork_nif PROPERTIES
            SUFFIX ".so"
            LINK_FLAGS "-undefined dynamic_lookup")
    elseif(UNIX)
        set_target_properties(clockwork_nif PROPERTIES SUFFIX ".so")
    endif()
    message(STATUS "BEAM NIF: enabled (clockwork.so)")
endif()

if(CLOCKWORK_LINK)
    target_link_libraries(clockwork PUBLIC Ableton::Link)
    target_compile_definitions(clockwork PUBLIC CLOCKWORK_LINK=1)
    # The audio half is opt-in on top. Tempo, transport and peers work with
    # CLOCKWORK_LINK alone.
    if(CLOCKWORK_LINK_AUDIO)
        target_compile_definitions(clockwork PUBLIC CLOCKWORK_WITH_LINK_AUDIO=1)
    endif()
    if(WIN32)
        # Link's Windows ThreadPriority uses the Multimedia Class Scheduler.
        target_link_libraries(clockwork PUBLIC avrt)
    endif()
endif()

if(WIN32)
    target_link_libraries(clockwork PUBLIC Ws2_32 winmm)
elseif(APPLE)
    target_link_libraries(clockwork PUBLIC
        "-framework CoreAudio" "-framework AudioUnit" "-framework Foundation"
        "-framework CoreFoundation" "-framework AVFoundation")
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(clockwork PUBLIC Threads::Threads rt ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(clockwork PRIVATE
        /W1 /wd4244 /wd4267 /wd4305 /wd4800 /wd4996 /permissive-)
else()
    target_compile_options(clockwork PRIVATE
        -Wno-deprecated-declarations -Wno-unused-parameter
        -fno-math-errno -fsigned-zeros)
endif()

# ─── Rust subsystems ─────────────────────────────────────────────────────────
# MIDI, gamepad and OSC networking are Rust, folded by cargo into the single
# `clockwork-native` umbrella staticlib (a binary may link exactly one Rust
# staticlib — each carries its own copy of std). The C++ boundaries are
# src/native/{MidiControl,GamepadControl,OscControl}.cpp against the headers in
# each crate's cpp/ directory.
if(CLOCKWORK_RUST)
    find_program(CARGO_EXECUTABLE cargo REQUIRED)
else()
    # A binary linked here would need the umbrella; with CLOCKWORK_RUST=OFF
    # only the embedder's own cargo build supplies the Rust half, and only
    # to its own binary. Refused here, as the device layer's dependants are.
    foreach(_needs_rust CLOCKWORK_STANDALONE BUILD_TESTS CLOCKWORK_PLUGINS CLOCKWORK_NIF)
        if(${_needs_rust})
            message(FATAL_ERROR
                "${_needs_rust}=ON needs CLOCKWORK_RUST=ON: it links a binary in this "
                "tree, and a binary needs the Rust subsystems. Build the library alone "
                "with -D${_needs_rust}=OFF, or let this tree build the umbrella.")
        endif()
    endforeach()
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CLOCKWORK_RUST_PROFILE debug)
    set(CLOCKWORK_RUST_CARGO_FLAGS)
else()
    set(CLOCKWORK_RUST_PROFILE release)
    set(CLOCKWORK_RUST_CARGO_FLAGS --release)
endif()
if(CLOCKWORK_CARGO_OFFLINE)
    list(APPEND CLOCKWORK_RUST_CARGO_FLAGS --offline)
endif()
# --locked always: rust/Cargo.lock is committed and is the dependency set that
# was tested. A Cargo.toml edit that would change it fails here instead of
# rewriting the lockfile as a side effect of a C++ build, and a distro build
# gets the reproducibility it requires without asking for it.
list(APPEND CLOCKWORK_RUST_CARGO_FLAGS --locked)

set(CLOCKWORK_RUST_FEATURES)
if(CLOCKWORK_MIDI)
    list(APPEND CLOCKWORK_RUST_FEATURES midi)
    # Windows takes midir's WinRT backend (full-length device names) plus the
    # WinRT DeviceWatcher for hot-swap; both live behind the `winrt` feature.
    if(WIN32)
        list(APPEND CLOCKWORK_RUST_FEATURES winrt)
    endif()
endif()
if(CLOCKWORK_GAMEPAD)
    list(APPEND CLOCKWORK_RUST_FEATURES gamepad)
endif()
if(CLOCKWORK_OSC)
    # `comms` carries the command-transport crate for the comms client library
    # (clockwork_comms below); the engine itself uses none of it.
    list(APPEND CLOCKWORK_RUST_FEATURES osc comms)
endif()
if(CLOCKWORK_SCHEDULER)
    list(APPEND CLOCKWORK_RUST_FEATURES schedule)
endif()
list(JOIN CLOCKWORK_RUST_FEATURES "," CLOCKWORK_RUST_FEATURES_ARG)
list(APPEND CLOCKWORK_RUST_CARGO_FLAGS --no-default-features)
if(CLOCKWORK_RUST_FEATURES_ARG)
    list(APPEND CLOCKWORK_RUST_CARGO_FLAGS --features ${CLOCKWORK_RUST_FEATURES_ARG})
endif()

# ── Which Rust staticlib ─────────────────────────────────────────────────────
#
# Exactly ONE Rust staticlib may be linked into a binary: each bundles its own
# copy of std, and two collide at link time. clockwork-native is the umbrella
# clockwork uses when it is the whole program.
#
# An embedder with Rust of its own therefore CANNOT link clockwork-native as well —
# it must build a single umbrella that depends on both clockwork's crates and
# its own, and point these three at it. This is why clockwork-native is "the umbrella
# clockwork uses standalone" rather than "the umbrella".
#
# The contract for a replacement, and the only thing clockwork assumes about
# it: it must accept the same feature names clockwork passes below (midi,
# gamepad, osc, schedule), forwarding each to the corresponding clockwork crate.
# Nothing here knows or cares what else it contains.
set(CLOCKWORK_RUST_WORKSPACE "${CLOCKWORK_RUST_DIR}" CACHE PATH
    "Cargo workspace holding the Rust umbrella to build")
set(CLOCKWORK_RUST_PACKAGE "clockwork-native" CACHE STRING
    "Cargo package name of the Rust umbrella")
set(CLOCKWORK_RUST_LIB_NAME "clockwork_native" CACHE STRING
    "Basename of the staticlib that package produces")

set(CLOCKWORK_RUST_LIB
    ${CLOCKWORK_RUST_WORKSPACE}/target/${CLOCKWORK_RUST_PROFILE}/${CMAKE_STATIC_LIBRARY_PREFIX}${CLOCKWORK_RUST_LIB_NAME}${CMAKE_STATIC_LIBRARY_SUFFIX})

# cargo tracks incremental freshness itself, so run it every build.
if(CLOCKWORK_RUST)
    add_custom_target(clockwork_rust ALL
        BYPRODUCTS ${CLOCKWORK_RUST_LIB}
        COMMAND ${CARGO_EXECUTABLE} build ${CLOCKWORK_RUST_CARGO_FLAGS} -p ${CLOCKWORK_RUST_PACKAGE}
        WORKING_DIRECTORY ${CLOCKWORK_RUST_WORKSPACE}
        COMMENT "Building Rust subsystems (cargo: ${CLOCKWORK_RUST_PACKAGE})"
        VERBATIM)
    add_dependencies(clockwork clockwork_rust)
endif()

# The Rust archive and the C++ host reference each other — Rust's scope plane
# calls back into the C++ shm base, the C++ control surfaces call into Rust —
# and one left-to-right link pass cannot resolve that. State the cycle as a
# target dependency so CMake repeats the archives.
add_library(clockwork_rust_lib STATIC IMPORTED)
set_target_properties(clockwork_rust_lib PROPERTIES IMPORTED_LOCATION ${CLOCKWORK_RUST_LIB})
target_link_libraries(clockwork_rust_lib INTERFACE clockwork)
target_link_libraries(clockwork PUBLIC clockwork_rust_lib)

if(WIN32)
    # Rust std (windows-msvc) pulls these in; authoritative list from
    # `rustc --print native-static-libs`.
    target_link_libraries(clockwork PUBLIC ntdll userenv dbghelp)
endif()

# Per-subsystem boundaries + the OS libraries each Rust backend needs at final link
# (Rust staticlibs do not carry their link deps).
if(CLOCKWORK_MIDI)
    target_sources(clockwork PRIVATE ${NATIVE_SRC}/MidiControl.cpp)
    target_include_directories(clockwork PUBLIC ${CLOCKWORK_RUST_DIR}/clockwork-midi/cpp)
    target_compile_definitions(clockwork PUBLIC CLOCKWORK_MIDI=1)
    if(APPLE)
        target_link_libraries(clockwork PUBLIC "-framework CoreMIDI")
    elseif(WIN32)
        target_link_libraries(clockwork PUBLIC ole32 oleaut32 runtimeobject)
    elseif(UNIX)
        target_link_libraries(clockwork PUBLIC asound)
    endif()
endif()
if(CLOCKWORK_GAMEPAD)
    target_sources(clockwork PRIVATE ${NATIVE_SRC}/GamepadControl.cpp)
    target_include_directories(clockwork PUBLIC ${CLOCKWORK_RUST_DIR}/clockwork-gamepad/cpp)
    target_compile_definitions(clockwork PUBLIC CLOCKWORK_GAMEPAD=1)
    if(APPLE)
        # gilrs cannot see Apple-driven pads; src/gc.rs uses GameController.
        target_link_libraries(clockwork PUBLIC "-framework GameController" objc)
    elseif(UNIX AND NOT WIN32)
        target_link_libraries(clockwork PUBLIC udev)   # gilrs evdev hotplug
    endif()
endif()
if(CLOCKWORK_OSC)
    target_sources(clockwork PRIVATE ${NATIVE_SRC}/OscControl.cpp)
    target_include_directories(clockwork PUBLIC ${CLOCKWORK_RUST_DIR}/clockwork-osc-net/cpp)
    target_compile_definitions(clockwork PUBLIC CLOCKWORK_OSC=1)
endif()

# The port substrate has no option and no boundary file. src/clockwork_ports.h is the
# contract and it lives with clockwork (Sam wrote it before the crate did);
# audio_processor.cpp calls clockwork_port_read/clockwork_port_write directly, so there is
# nothing to switch off — with no port bound the two calls scan an empty table
# and return. Only the DISK endpoints' header needs an include path, because it
# ships with the crate that implements it, the way every other Rust subsystem's
# does.
target_include_directories(clockwork PUBLIC ${CLOCKWORK_RUST_DIR}/clockwork-ports-disk/cpp)

# The timed-event store has no option and no boundary file either, for the same
# reason: src/scheduler/Scheduler.h and src/scheduler/schedule_parse.h are the
# contract and they live with clockwork, and what is behind them
# (rust/clockwork-schedule) is reached by those headers directly. Only the C
# ABI header needs an include path.
# Unconditional, and not an oversight: the crate's `parse` half is compiled in
# both configurations (a timestamped bundle is the DSP's, so a build with no
# store still has to read its timetag), and one header declares both halves.
# With the store off its store functions are simply not in the archive, and
# every C++ site that would call one is gated — so a mistake there is a link
# error, which is the failure you want.
target_include_directories(clockwork PUBLIC ${CLOCKWORK_RUST_DIR}/clockwork-schedule/cpp)
# The clock's value type and follower registry (src/clock/Timeline.h and
# MidiTimelines.h are thin C++ over it).
target_include_directories(clockwork PUBLIC ${CLOCKWORK_RUST_DIR}/clockwork-clock/cpp)

# ─── Plugin hosting adapters ─────────────────────────────────────────────────
# Each format's directory adds its adapter to clockwork and builds its own
# test plugin. They are added here, after clockwork exists, so each one keeps
# its own sources, its own SDK fetch and its own flags in its own file.
if(CLOCKWORK_PLUGINS AND NOT EMSCRIPTEN)
    add_subdirectory(plugins/clap)
    add_subdirectory(plugins/vst3)
endif()

# ─── The DSP ─────────────────────────────────────────────────────────────────
# Everything above is clockwork. Everything the DSP owns is behind
# src/dsp_api.h — the entry points docs/BOUNDARY.md lists — and arrives as one
# more static library named by CLOCKWORK_DSP. The count lived here too and went
# stale: dsp_asset was added and this comment still said seven. The header is
# the list; nothing else should restate its length.
if(CLOCKWORK_DSP_DIR)
    get_filename_component(CLOCKWORK_DSP_SRC_DIR "${CLOCKWORK_DSP_DIR}"
                           ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
else()
    set(CLOCKWORK_DSP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dsp/${CLOCKWORK_DSP}")
endif()
if(NOT EXISTS "${CLOCKWORK_DSP_SRC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "CLOCKWORK_DSP=${CLOCKWORK_DSP}: no CMakeLists.txt at ${CLOCKWORK_DSP_SRC_DIR}. "
        "A DSP is a directory defining a target of its own name that implements "
        "src/dsp_api.h; set CLOCKWORK_DSP_DIR to point at one outside this tree.")
endif()
# The explicit binary dir is required, not stylistic: add_subdirectory refuses
# a source directory outside the current tree without one.
add_subdirectory("${CLOCKWORK_DSP_SRC_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/dsp-${CLOCKWORK_DSP}")
if(NOT TARGET ${CLOCKWORK_DSP})
    message(FATAL_ERROR "dsp/${CLOCKWORK_DSP} defined no target named ${CLOCKWORK_DSP}")
endif()
message(STATUS "DSP: ${CLOCKWORK_DSP}")

# Clockwork and the DSP call each other in both directions by construction —
# clockwork runs blocks and hands over OSC, the DSP answers through
# DspHost::emit_osc and DspHost::log. Link them as a group so a single pass is
# not asked to resolve a cycle.
if(APPLE OR MSVC)
    set(CLOCKWORK_LIB_GROUP clockwork ${CLOCKWORK_DSP} ${CLOCKWORK_RUST_LIB})
else()
    # $<TARGET_FILE:clockwork>, not the target NAME. CMake reorders a named
    # target by its own dependency rules and hoisted libtau.a OUT of
    # the group — so the Rust staticlib, still inside it, could not reach back
    # for the C++ symbols it calls (clockwork_shm_base, clockwork_scope_geometry) and the
    # link failed. A file path stays where it is put. The DSP was already
    # pinned this way; clockwork was not.
    # BOTH forms of clockwork, deliberately. The target carries the usage
    # requirements — include directories and compile definitions — which a
    # bare file path does not; $<TARGET_FILE:> pins the archive INSIDE the
    # group, which a target name does not, because CMake reorders named
    # targets by its own dependency rules and hoisted this one out. Without
    # the file, the Rust staticlib cannot reach back for the C++ symbols it
    # calls; without the target, nothing that links this can find a header.
    # The archive appearing twice on the line costs nothing.
    set(CLOCKWORK_LIB_GROUP
        clockwork
        -Wl,--start-group
        $<TARGET_FILE:clockwork> $<TARGET_FILE:${CLOCKWORK_DSP}> ${CLOCKWORK_RUST_LIB}
        -Wl,--end-group)
endif()

# What an EMBEDDER links. CLOCKWORK_LIB_GROUP above is a plain variable in
# this directory's scope, so it is empty in a parent project — a consumer that
# used it got clockwork with no DSP in the link line and a page of undefined
# dsp_* references. An INTERFACE target carries the same flags across the
# directory boundary, including the --start-group/--end-group pair
# the clockwork/DSP call cycle needs.
#
# Link this, and only this:  target_link_libraries(you PRIVATE clockwork_bundle)
#
# A build that cannot use the library at all — freestanding or embedded — takes
# CLOCKWORK_CORE_SOURCES and CLOCKWORK_INCLUDE_DIRS instead; see where they are set.

add_library(clockwork_bundle INTERFACE)
target_link_libraries(clockwork_bundle INTERFACE ${CLOCKWORK_LIB_GROUP})
# The group names the DSP by FILE so it stays inside --start-group, and a file
# path creates no dependency edge — so a consumer linking the bundle got
# "No rule to make target" for the DSP's archive unless it happened to build
# the guest first. The bundle carries that ordering itself now.
add_dependencies(clockwork_bundle ${CLOCKWORK_DSP})
if(CLOCKWORK_RUST)
    add_dependencies(clockwork_bundle clockwork_rust)
endif()

# ── Windows version resource ─────────────────────────────────────────────────
# src/native/clockwork_version.rc.in existed as a template that nothing configured, so
# a Windows build carried NO version information at all: Explorer showed a bare
# filename, and so did every UAC and device-permission dialog. It is generated
# now, and attached to clockwork_bundle as an INTERFACE source so an
# EMBEDDER's executable gets it without knowing it exists — the same reason the
# link group is a target rather than a variable.
if(WIN32)
    # Windows wants the version as four comma-separated fields.
    string(REPLACE "." ";" CLOCKWORK_VERSION_PARTS "${CLOCKWORK_PRODUCT_VERSION}")
    list(LENGTH CLOCKWORK_VERSION_PARTS CLOCKWORK_VERSION_N)
    while(CLOCKWORK_VERSION_N LESS 4)
        list(APPEND CLOCKWORK_VERSION_PARTS 0)
        list(LENGTH CLOCKWORK_VERSION_PARTS CLOCKWORK_VERSION_N)
    endwhile()
    list(JOIN CLOCKWORK_VERSION_PARTS "," CLOCKWORK_RC_VERSION_COMMAS)
    set(PROJECT_VERSION "${CLOCKWORK_PRODUCT_VERSION}")

    # The product's icon, for the window class and the taskbar: resource id
    # 1 when the embedder names an .ico (CLOCKWORK_PRODUCT_ICON), nothing
    # otherwise — the rc is fine with an empty line there.
    set(CLOCKWORK_PRODUCT_ICON "" CACHE FILEPATH "An .ico this product's binaries carry as resource 1 (Windows)")
    set(CLOCKWORK_RC_ICON "")
    if(CLOCKWORK_PRODUCT_ICON AND EXISTS "${CLOCKWORK_PRODUCT_ICON}")
        file(TO_CMAKE_PATH "${CLOCKWORK_PRODUCT_ICON}" _clockwork_icon)
        set(CLOCKWORK_RC_ICON "1 ICON \"${_clockwork_icon}\"")
    endif()
    set(CLOCKWORK_VERSION_RC "${CMAKE_CURRENT_BINARY_DIR}/clockwork_version.rc")
    configure_file("${NATIVE_SRC}/clockwork_version.rc.in" "${CLOCKWORK_VERSION_RC}" @ONLY)
    target_sources(clockwork_bundle INTERFACE "${CLOCKWORK_VERSION_RC}")
endif()

# There is no reference host: an engine is a library with no process shape
# of its own. The process around it — the command line, the sockets, the run
# loop — is the client's (clockwork-supersonic/host/ is one), composed from
# the comms client library below.

# ─── The standalone SCHEDULER host ───────────────────────────────────────────
# A different program from the one above: an OSC/MIDI event scheduler with no
# synthesis engine, driving the timed store off the wall clock. lanes.h points
# at this shape as what a minimal host looks like.
#
# BUILT NOW, and that is the whole change. Its own header said "not built here:
# no target in CMakeLists.txt compiles this file" — five files describing a
# program this tree did not produce, so nothing could tell whether they still
# compiled against a boundary that had moved underneath them. A reference host that
# is not built is not a reference, it is a document that looks like code.
#
# Needs the timed store, because scheduling is the entire program: with
# CLOCKWORK_SCHEDULER=OFF there is nothing for it to be.
if(CLOCKWORK_STANDALONE AND CLOCKWORK_SCHEDULER)
    add_executable(clockwork-scheduler-host ${CLOCKWORK_SRC}/host/main.cpp)
    add_dependencies(clockwork-scheduler-host ${CLOCKWORK_DSP})
    target_link_libraries(clockwork-scheduler-host PRIVATE ${CLOCKWORK_LIB_GROUP})
endif()

# ─── The comms client library ────────────────────────────────────────────────
# The command transports (src/comms/clockwork_comms.h). NOT part of the engine:
# an engine has no sockets, and a host that wants some links this and stands
# them behind the engine's one reply door. SuperSonic's host and the NIF both
# do; an in-process embedder need not. Its Rust half is
# the clockwork-comms crate, carried in the native umbrella under the `comms`
# feature so the symbols are there for whoever links this.
add_library(clockwork_comms STATIC
    ${CLOCKWORK_SRC}/comms/UdpOscTransport.cpp
    ${CLOCKWORK_SRC}/comms/StreamOscTransport.cpp
    ${CLOCKWORK_SRC}/comms/UdsDgramOscTransport.cpp
    ${CLOCKWORK_SRC}/comms/ShmTransport.cpp)
if(CLOCKWORK_FLOAT_FLAGS)
    target_compile_options(clockwork_comms PUBLIC ${CLOCKWORK_FLOAT_FLAGS})
endif()
target_include_directories(clockwork_comms PUBLIC ${CLOCKWORK_SRC}/comms)
target_link_libraries(clockwork_comms PUBLIC clockwork)
set_target_properties(clockwork_comms PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The old name, for consumers pinned to it; retire once they say clockwork_comms.
add_library(clockwork_transports ALIAS clockwork_comms)

# ─── The plugin-host dispatcher ──────────────────────────────────────────────
# The adapters themselves were added further up, where clockwork is built.
# This is the boundary on top of them: one library holding the public C symbols.
if(CLOCKWORK_PLUGINS)
    # plugin_host.cpp is the ONE definition of plugin_host.h's C symbols; the
    # adapters export namespaced entry points instead, because two adapters
    # both defining plugin_scan is a duplicate symbol at link.
    if(NOT TARGET clockwork_plugin_host)
        add_library(clockwork_plugin_host STATIC ${CLOCKWORK_SRC}/plugin_host.cpp)
        target_include_directories(clockwork_plugin_host PUBLIC ${CLOCKWORK_SRC})
        set_target_properties(clockwork_plugin_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    # Tracks: the consumer that gives a hosted plugin a place in the audio
    # path. Built with the host rather than into clockwork, so a build
    # without plugin support has neither and the audio path's hook stays null.
    target_sources(clockwork_plugin_host PRIVATE ${CLOCKWORK_SRC}/plugin_track.cpp)
    # Anything linking the host learns it is there, so a host's main can install
    # the tracks under a define that is actually set somewhere.
    target_compile_definitions(clockwork_plugin_host PUBLIC CLOCKWORK_HAS_PLUGIN_TRACKS=1)
    # Discovery: finding plugins on disk, so a host does not ship with paths
    # compiled into it. Built here rather than into clockwork for the same
    # reason as the tracks — a build without plugin support has neither.
    target_sources(clockwork_plugin_host PRIVATE ${CLOCKWORK_SRC}/plugin_discovery.cpp)
    # The editor window: the engine's own, so the editor belongs to the instance
    # that makes sound rather than to a second copy loaded to draw with. macOS
    # relies on the Cocoa run loop the bridge already pumps; Windows on the
    # message pump it already runs (PluginBridgeMain.cpp, pumpEvents), which
    # is why neither needs a thread of its own. Linux has no loop to lean on
    # and a stub answers "no window" honestly.
    if(APPLE)
        target_sources(clockwork_plugin_host PRIVATE ${CLOCKWORK_SRC}/native/PluginEditorWindow.mm)
        target_link_libraries(clockwork_plugin_host PRIVATE "-framework Cocoa")
    elseif(WIN32)
        # Not yet compiled or run on Windows: see the file's header.
        target_sources(clockwork_plugin_host PRIVATE ${CLOCKWORK_SRC}/native/PluginEditorWindowWin.cpp)
        # user32 for the window itself. PRIVATE on a static library still
        # reaches whoever links it, so the bridge and the tests get it too.
        target_link_libraries(clockwork_plugin_host PRIVATE user32)
    else()
        target_sources(clockwork_plugin_host PRIVATE ${CLOCKWORK_SRC}/plugin_editor_window_stub.cpp)
    endif()
    if(TARGET clockwork_plugin_vst3)
        target_link_libraries(clockwork_plugin_host PUBLIC clockwork_plugin_vst3)
    endif()
    if(TARGET clockwork_plugin_clap)
        target_link_libraries(clockwork_plugin_host PUBLIC clockwork_plugin_clap)
    endif()

    # The verbs: the /clockwork-sys/track/* surface, decoded where the plugins are.
    # Part of the host library because both the bridge and a test that hosts
    # in-process want them, and neither wants the other's main().
    target_sources(clockwork_plugin_host PRIVATE ${CLOCKWORK_SRC}/plugin_bridge_verbs.cpp)
    # The verbs speak OSC (clockwork_sys.h, oscpack) and clockwork_product.h: clockwork's
    # headers, not its library.
    target_include_directories(clockwork_plugin_host PRIVATE ${CLOCKWORK_INCLUDE_DIRS})
    target_compile_definitions(clockwork_plugin_host PRIVATE CLOCKWORK_PRODUCT_NAME="${CLOCKWORK_PRODUCT_NAME}")

    # ── The bridge ──────────────────────────────────────────────────────────
    # THE ENGINE NEVER LINKS THE PLUGIN HOST. Plugins run in this separate
    # process, which shares a memory segment with the engine (plugin_bridge.h)
    # and is reached through a pair of ports like any other outboard audio.
    # A plugin that crashes takes the bridge down and the engine notices, so
    # the audio keeps going; a plugin's editor window belongs to the bridge's
    # own run loop. clockwork gets only TrackControl — the proxy that owns
    # the segment, spawns the bridge and relays OSC across — and the define
    # that lets a host's main and ClockworkEngine wire it in.
    #
    # The binary sits beside the host binary under a product-facing name
    # (CLOCKWORK_PLUGIN_BRIDGE_NAME), because it is what a user sees in Activity
    # Monitor when a plugin misbehaves, and what an entitlement is granted to.
    target_sources(clockwork PRIVATE ${NATIVE_SRC}/TrackControl.cpp)
    target_compile_definitions(clockwork PUBLIC
        CLOCKWORK_HAS_PLUGIN_TRACKS=1
        CLOCKWORK_PLUGIN_BRIDGE_NAME="${CLOCKWORK_PLUGIN_BRIDGE_NAME}")
    if(CLOCKWORK_PLUGIN_BRIDGE_DIR)
        target_compile_definitions(clockwork PUBLIC
            CLOCKWORK_PLUGIN_BRIDGE_DIR="${CLOCKWORK_PLUGIN_BRIDGE_DIR}")
    endif()

    if(NOT EMSCRIPTEN)
        # The bridge's own Rust staticlib: only the port substrate. It cannot
        # share clockwork-native, whose archive is bound to clockwork by the link
        # cycle above, and it wants none of the engine's subsystems anyway.
        # The profile and offline flags, not the feature list: the bridge's
        # crate has no subsystems to switch on.
        set(CLOCKWORK_BRIDGE_CARGO_FLAGS)
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            list(APPEND CLOCKWORK_BRIDGE_CARGO_FLAGS --release)
        endif()
        if(CLOCKWORK_CARGO_OFFLINE)
            list(APPEND CLOCKWORK_BRIDGE_CARGO_FLAGS --offline)
        endif()
        list(APPEND CLOCKWORK_BRIDGE_CARGO_FLAGS --locked)
        # Built from Clockwork's workspace whoever is embedding: the bridge
        # is clockwork's binary, and an embedder's umbrella has nothing it
        # needs.
        set(CLOCKWORK_BRIDGE_RUST_LIB
            ${CLOCKWORK_RUST_DIR}/target/${CLOCKWORK_RUST_PROFILE}/${CMAKE_STATIC_LIBRARY_PREFIX}clockwork_bridge_native${CMAKE_STATIC_LIBRARY_SUFFIX})
        add_custom_target(clockwork_bridge_rust ALL
            BYPRODUCTS ${CLOCKWORK_BRIDGE_RUST_LIB}
            COMMAND ${CARGO_EXECUTABLE} build ${CLOCKWORK_BRIDGE_CARGO_FLAGS} -p clockwork-bridge-native
            WORKING_DIRECTORY ${CLOCKWORK_RUST_DIR}
            COMMENT "Building Rust port substrate for the plugin bridge (cargo: clockwork-bridge-native)"
            VERBATIM)
        add_library(clockwork_bridge_rust_lib STATIC IMPORTED)
        set_target_properties(clockwork_bridge_rust_lib PROPERTIES IMPORTED_LOCATION ${CLOCKWORK_BRIDGE_RUST_LIB})

        add_executable(clockwork_plugin_bridge
            ${NATIVE_SRC}/PluginBridgeMain.cpp
            ${NATIVE_SRC}/RealtimeThread.cpp
            ${OSCPACK_SOURCES})
        if(APPLE)
            target_sources(clockwork_plugin_bridge PRIVATE ${NATIVE_SRC}/CocoaEventPump.mm)
        elseif(WIN32)
            # Its own version and icon: the editor windows it opens carry
            # this binary's, and consent prompts name it.
            target_sources(clockwork_plugin_bridge PRIVATE "${CLOCKWORK_VERSION_RC}")
        endif()
        add_dependencies(clockwork_plugin_bridge clockwork_bridge_rust)
        target_include_directories(clockwork_plugin_bridge PRIVATE ${CLOCKWORK_INCLUDE_DIRS})
        target_compile_definitions(clockwork_plugin_bridge PRIVATE
            CLOCKWORK_PRODUCT_NAME="${CLOCKWORK_PRODUCT_NAME}"
            CLOCKWORK_HAS_PLUGIN_TRACKS=1)
        target_link_libraries(clockwork_plugin_bridge PRIVATE clockwork_plugin_host clockwork_bridge_rust_lib)
        if(APPLE)
            target_link_libraries(clockwork_plugin_bridge PRIVATE
                "-framework Cocoa" "-framework CoreFoundation")
            # An app bundle, <CLOCKWORK_PLUGIN_BRIDGE_NAME>.app beside the host
            # binary, so the process has a bundle identifier (see
            # CLOCKWORK_PLUGIN_BRIDGE_BUNDLE_ID). The engine looks inside it:
            # TrackControl::findBridge. LSUIElement keeps it out of the Dock.
            set_target_properties(clockwork_plugin_bridge PROPERTIES
                MACOSX_BUNDLE TRUE
                MACOSX_BUNDLE_INFO_PLIST "${NATIVE_SRC}/bridge_info.plist.in")
        elseif(WIN32)
            target_link_libraries(clockwork_plugin_bridge PRIVATE ntdll userenv dbghelp ws2_32)
        else()
            target_link_libraries(clockwork_plugin_bridge PRIVATE pthread ${CMAKE_DL_LIBS})
        endif()
        set_target_properties(clockwork_plugin_bridge PROPERTIES
            OUTPUT_NAME "${CLOCKWORK_PLUGIN_BRIDGE_NAME}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        # Anything that builds the host binary gets the bridge beside it.
    endif()
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
# Clockwork's own suite, and only when clockwork is what is being built.
# An embedder turning BUILD_TESTS on means ITS tests, not ours.
if(BUILD_TESTS AND CLOCKWORK_STANDALONE)
    enable_testing()
    add_subdirectory(test)
endif()
