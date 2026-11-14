# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2025-2026 Sam Aaron
#
# Clockwork's own tests. Everything here pins something Clockwork owns —
# ring framing, the lanes ABI, the shared-memory segment, scheduler tags,
# clock arithmetic, device policy, recording, the /clockwork/ prefix rule and
# the audio path itself. Nothing here tests a DSP: what makes sound belongs to
# whichever project embeds clockwork, and its tests belong there too.

# Prefer a system Catch2 v3 (distro package-builds run offline and Debian ships
# one); fall back to the pinned FetchContent copy for dev machines without it.
find_package(Catch2 3 QUIET)
if(NOT TARGET Catch2::Catch2)
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.2
        GIT_SHALLOW    TRUE
        ${_ss_fc_exclude})   # keep Catch2's install() rules out of our manifest
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
else()
    message(STATUS "Catch2: system install ${Catch2_VERSION}")
    list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
endif()

# The headers a host includes, compiled by a target that cannot see JUCE: it
# takes what clockwork exports to a host — its interface include directories
# and definitions, and Smoothie's worklet — but not smoothie's modules, which
# are where JUCE lives. A JUCE include in any of them fails this build.
add_library(clockwork_juce_free_headers OBJECT juce_free_headers.cpp)
target_include_directories(clockwork_juce_free_headers PRIVATE
    $<FILTER:$<TARGET_PROPERTY:clockwork,INTERFACE_INCLUDE_DIRECTORIES>,EXCLUDE,smoothie/modules>
    ${CLOCKWORK_SRC}/comms ${CLOCKWORK_SRC}/../smoothie/worklet)
target_compile_definitions(clockwork_juce_free_headers PRIVATE
    $<TARGET_PROPERTY:clockwork,INTERFACE_COMPILE_DEFINITIONS>)
target_compile_features(clockwork_juce_free_headers PRIVATE cxx_std_20)

add_executable(clockwork_tests
    test_main.cpp

    # Shared fixtures.
    OscTestUtils.cpp

    # The rings and the lanes ABI — how bytes get in and out.
    test_ring_writer.cpp
    test_ring_buffer_write.cpp
    test_ring_exhaustive.cpp
    test_ring_reader.cpp
    test_wait_word.cpp
    test_os_pages.cpp
    test_endpoint_requests.cpp
    test_timetag.cpp
    test_ring_concurrency.cpp
    test_lanes.cpp
    test_arena.cpp
    test_scope.cpp

    # How often one kind of log line is said.
    test_log_limit.cpp
    # How long a stretch of renders took, for the [load] line.
    test_render_spread.cpp

    # OSC: the codec, the ingress router, and the one prefix clockwork keeps.
    test_osc_codec.cpp
    test_oscpack.cpp
    test_osc_ingress.cpp
    test_clockwork_sys.cpp
    test_host_forward.cpp
    test_inbound_events.cpp

    # Time: the wire forms and the arithmetic underneath. (The schedule
    # store's own contract, test_scheduler.cpp, is added below only when the
    # store is built — see CLOCKWORK_SCHEDULER.)
    test_schedule_parse.cpp
    test_clock_math.cpp
    # What a tempo change does to the beat that is playing. Runs against
    # whichever backing is compiled — the session-of-one or Link — so both
    # are held to the same answer.
    test_clock_tempo_change.cpp
    # The ClockworkClockState write protocol: ordering and the re-anchor, on the
    # struct's own mutators, with no sleeping.
    test_clock_state.cpp
    test_clockwork_clock.cpp
    # One snapshot, one domain: ClockworkClock::timeline(id) and the rpc verbs that
    # answer from it. The follower registry behind the midi ids is tested in
    # Rust (rust/clockwork-clock).
    test_timeline.cpp
    test_link_audio_host.cpp
    # A peer's stream placed on the timeline: the vendored renderer, fed
    # buffers by hand on a session of one. Compiles to nothing without Link Audio.
    test_link_audio_input_renderer.cpp
    test_block_stamp.cpp
    test_time_source.cpp
    test_metrics_history.cpp

    # The shared-memory segment: layout guard and permissions.
    test_shm_segment.cpp

    # The client's pool over the assets area lane.
    test_asset_pool.cpp
    test_osc_front.cpp
    test_egress_router.cpp

    # The build target's own capability profile.
    test_platform.cpp
    test_clockwork_path.cpp

    # Placement: the host-supplied arena that lets one allocation path serve
    # every target, including the one that cannot use the system allocator.
    test_mem_arena.cpp

    # Devices and recording. test_fuzzy_match is string matching and needs no
    # device; the other two are added below, with the device layer.
    test_fuzzy_match.cpp

    # The boundary itself, driven end to end through the lanes ABI, one file per
    # shape it moves data in: whole blocks of audio in and out
    # (test_audio_path), messages and replies (test_dsp_boundary), and — at the end
    # of this list — the shared-memory regions (test_dsp_regions). Plus the
    # audio thread's no-allocation rule while it does all of it
    # (test_rt_alloc). LanesFixture boots clockwork once for the binary and
    # drives it a block at a time — no audio device, no threads.
    LanesFixture.cpp
    test_audio_path.cpp
    test_dsp_boundary.cpp
    # The parameter table (docs/PARAMS.md), through the lanes ABI; test_params_engine.cpp below on a native engine.
    test_params.cpp
    test_rt_alloc.cpp
    # The origin-token reply path, the ring wire against its golden corpus,
    # the shm segment as a trust boundary, the graph-robustness policy, and
    # the command transports against real sockets.
    test_reply_routing.cpp
    test_sink_profile.cpp
    test_ring_wire_conformance.cpp
    test_server_shm_security.cpp
    test_transports.cpp

    # The port substrate (rust/clockwork-ports) through its C ABI, its two
    # disk endpoints (rust/clockwork-ports-disk), and the boundary they meet the
    # DSP on. The ring's own arithmetic and its no-allocation rule are pinned
    # in Rust, where the allocator can actually be seen.
    test_ports.cpp
    test_samples_through_engine.cpp

    # Reading and writing audio files (src/clockwork_audio_file.h): the
    # decoders, this repository's FLAC encoder, and the versioned structs
    # they fill. Needs no device and no engine.
    test_audio_file.cpp
    test_audio_fixtures.cpp

    # What the product is called and the banner it boots with
    # (src/clockwork_product.h): clockwork's own by default, the product's
    # through its header. The override unit is compiled with such a header.
    test_product_banner.cpp
    test_product_banner_override.cpp

    # Which channels are which: the map a guest reads through
    # DspConfig::channel_map and a client through the segment header. Binds and
    # unbinds real ports, so it leaves the bus as it found it.
    test_channel_map.cpp

    # Events leaving clockwork (rust/clockwork-sinks) through the C ABI of
    # src/clockwork_event_sink.h, and through DspHost's open_sink/send_sink — the
    # path a DSP that is its own scheduler uses. The drain's own arithmetic
    # (time order, lateness, the cost of a refusal, the bytes on a real
    # socket) is pinned in Rust, where an allocator can be counted and a
    # loopback socket can be read.
    #
    # LAST, for the same reason test_stream_identity.cpp is next to last: its
    # final case rebuilds the DSP too, to see clockwork close the sinks that
    # DSP opened.
    test_event_sinks.cpp

    # The five shared-memory regions DspConfig hands the guest, each with its
    # own writer and its own promises. LAST, for the same reason the two files
    # above it are late: its final case rebuilds the DSP, to see one region
    # survive that and another be cleared by it — which is the whole difference
    # between DspConfig::persistent and DspConfig::guest_memory.
    test_dsp_regions.cpp
    # The guest keeps to the regions it was handed: a fence under
    # AddressSanitizer, proved by a child that must die.
    test_guest_fence.cpp
    test_restarts.cpp
    # The least a restart does, as a rule alone; the hosts that ask it are
    # pinned in test_restarts.cpp and test_device_policy.cpp.
    test_restart_plan.cpp

    # Large payloads across the boundary, both ways — and the claim that there is
    # no separate door for them in either direction. Also fixes the capacity
    # asymmetry (ingress ring 6x the RT egress ring) as a measured fact.
    test_dsp_blobs.cpp

    # The guest to client audio-publishing region (shm_tap_stream.hpp): the
    # ring a waveform display reads. Header-only and lock-free, so both halves
    # of the contract are testable here without a device or a guest.
    test_tap_stream.cpp

    # The audio capture region (shm_audio_buffer.hpp): the rolling window a
    # recorder or a meter pulls from. Its real contract is what happens when
    # the reader falls behind, which is the case these pin hardest.
    test_shm_audio_buffer.cpp

    # MIDI clock out. Not gated on CLOCKWORK_SCHEDULER: the 24 pulses a beat
    # are ClockworkClock's business and arrive whether or not this build
    # compiled the timed queue.
    test_midi_clock_out.cpp
)
# The override unit is what a product's build looks like: the product header
# named on the command line, everything else untouched.
set_source_files_properties(test_product_banner_override.cpp PROPERTIES COMPILE_DEFINITIONS
    "CLOCKWORK_PRODUCT_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/product_header_fixture.h\"")

# Hosting plugins (CLOCKWORK_PLUGINS=ON only). Each format's tests come with
# its adapter and its in-tree test plugin; with the option off none of this is
# compiled and the suite is exactly what it was.
if(TARGET clockwork_plugin_vst3)
    target_sources(clockwork_tests PRIVATE test_plugin_vst3.cpp)
    # Discovery is format-agnostic, but its cases need a plugin that is
    # certainly present, and the in-tree VST3 test bundle is the only one.
    target_sources(clockwork_tests PRIVATE test_plugin_discovery.cpp)
    # Tracks: names, lanes, chain order, handles, notes and controllers, rig
    # round-trips, and what a block sounds like through one. Needs a plugin
    # whose output is known, which is the in-tree one.
    target_sources(clockwork_tests PRIVATE test_plugin_track.cpp)
    target_link_libraries(clockwork_tests PRIVATE
        clockwork_plugin_host clockwork_vst3_pluginterfaces)
    add_dependencies(clockwork_tests ClockworkTestGain)
    target_compile_definitions(clockwork_tests PRIVATE
        CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE="${CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE}"
        CLOCKWORK_VST3_TEST_PLUGIN_SO="$<TARGET_FILE:ClockworkTestGain>")
    # How a follower timeline reaches a track: the seqlock its snapshot
    # crosses the segment under, and the engine-side rewrite that puts a
    # resolved id on the bind verb. Pure, so it needs no engine and no bridge.
    target_sources(clockwork_tests PRIVATE test_track_timeline.cpp)
    # Where the bridge is found: the host's configured path, the environment
    # override, the search beside the executable. Pure; no bridge is run.
    target_sources(clockwork_tests PRIVATE test_track_bridge_path.cpp)
    # The bridge as a process: these cases stand in for the engine, spawn the
    # real binary over a segment they made, and read back what it rendered.
    if(TARGET clockwork_plugin_bridge)
        target_sources(clockwork_tests PRIVATE test_plugin_bridge.cpp)
        add_dependencies(clockwork_tests clockwork_plugin_bridge)
        target_compile_definitions(clockwork_tests PRIVATE
            CLOCKWORK_PLUGIN_BRIDGE_EXE="$<TARGET_FILE:clockwork_plugin_bridge>")
    endif()
endif()

# The cross-format parity case only exists when BOTH test plugins do: it
# asserts the two formats produce identical audio through one abstraction,
# which is the claim neither format's own suite can make about itself.
if(TARGET clockwork_plugin_vst3 AND TARGET clockwork_plugin_clap)
    target_sources(clockwork_tests PRIVATE test_plugin_parity.cpp)
endif()

# ─── The timed store, or its absence ─────────────────────────────────────────
# The two configurations are genuinely different products and each gets the
# suite that applies to it. test_scheduler.cpp pins the store's contract and
# cannot be compiled without one; test_scheduler_absent.cpp pins what a build
# with no store does instead, and cannot be compiled with one. Neither is
# skipped at runtime: a case that reports "passed" because it never ran
# proves nothing.
if(CLOCKWORK_SCHEDULER)
    target_sources(clockwork_tests PRIVATE test_scheduler.cpp)
    # The standalone scheduler host's logic (src/host/host_scheduler.h): the
    # control verbs parsed into the store, and due events dispatched through
    # the host's own route table to its OSC and MIDI leaves. Nothing else
    # compiles those headers, so this is also what keeps them compiling.
    target_sources(clockwork_tests PRIVATE test_host_scheduler.cpp)
else()
    target_sources(clockwork_tests PRIVATE test_scheduler_absent.cpp)
endif()

# Device policy exists only when the device layer does: DevicePolicy is
# smoothie/JUCE. See CLOCKWORK_DEVICE.
if(CLOCKWORK_DEVICE)
    target_sources(clockwork_tests PRIVATE
        test_device_policy.cpp
        test_audio_recovery.cpp
        test_realtime_thread.cpp)
endif()

target_include_directories(clockwork_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_tests PRIVATE ${CLOCKWORK_LIB_GROUP} clockwork_comms Catch2::Catch2)
target_compile_definitions(clockwork_tests PRIVATE
    CLOCKWORK_TESTS_BINARY="$<TARGET_FILE:clockwork_tests>"
    CLOCKWORK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    RING_WIRE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ring_wire.txt"
    CLOCKWORK_AUDIO_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/audio")

target_compile_definitions(clockwork_tests PRIVATE
    JUCE_APPLICATION_NAME_STRING="clockwork-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(clockwork_tests PRIVATE /W1 /wd4244 /wd4267 /wd4305 /permissive-)
endif()


# ─── Plugin hosting ──────────────────────────────────────────────────────────
# Only when CLOCKWORK_PLUGINS built the format's in-tree test plugin. Each
# format's tests drive src/plugin_host.h and nothing format-specific; the
# plugin they load is found by path, built into <build>/plugins.
if(TARGET clockwork_clap_gain)
    target_sources(clockwork_tests PRIVATE test_plugin_clap.cpp)
    add_dependencies(clockwork_tests clockwork_clap_gain)
    target_link_libraries(clockwork_tests PRIVATE clockwork_plugin_clap)
    target_compile_definitions(clockwork_tests PRIVATE
        CLOCKWORK_CLAP_TEST_PLUGIN="$<TARGET_FILE:clockwork_clap_gain>")
    # One case crosses into plugin_host.h's own C symbols, which only exist
    # where the dispatcher is linked. Guarded so the CLAP tests stand alone
    # when it is not.
    if(TARGET clockwork_plugin_host)
        target_link_libraries(clockwork_tests PRIVATE clockwork_plugin_host)
        target_compile_definitions(clockwork_tests PRIVATE CLOCKWORK_CLAP_TEST_HOST_API=1)
    endif()
endif()

include(CTest)
include(Catch)
# Discovery reads the test list as UTF-8 (test/cmake/CatchAddTestsUtf8.cmake.in
# says why): Catch2's own script, included under the policy that makes CMake
# decode a process's output that way on Windows.
set(CATCH_ADD_TESTS_SCRIPT "${_CATCH_DISCOVER_TESTS_SCRIPT}")
configure_file(cmake/CatchAddTestsUtf8.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/CatchAddTestsUtf8.cmake" @ONLY)
set(_CATCH_DISCOVER_TESTS_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/CatchAddTestsUtf8.cmake"
    CACHE INTERNAL "Catch2's CatchAddTests.cmake, run with its output read as UTF-8" FORCE)
# One CTest entry per Catch2 case; CI runs the suite through ctest as well as
# through the binaries (.github/workflows/ci.yml says why). A case that SKIPs
# (the impossible-heap case does under ASan) is the only case its process
# runs, so the process exits 4, Catch2's "everything was skipped"; Catch2
# 3.5's discovery doesn't tell ctest that, which then reports it failed.
#
# A CASE'S NAME MUST NOT OPEN A SQUARE BRACKET IT DOES NOT CLOSE. The
# discovered names travel through a CMake list, where an unbalanced `[`
# swallows every `;` up to the next `]`, fusing the case and the ones after
# it into a single entry that matches nothing. Tags are fine; prose is not.
#
# Most cases are pumped by the test thread on a sample-count clock
# (EngineFixture::defaultConfig) and depend on no thread's timing, so they run
# in parallel. A case whose claim is about a live audio thread, or about real
# traffic arriving while nobody pumps, boots the headless driver
# (EngineFixture::driverConfig) and is tagged [rt]: labelled `rt`, it takes
# turns with the others so tagged. CI runs them in a step of their own, one at
# a time, beside nothing (`ctest -L rt -j 1`), and everything else with
# `-LE rt`. Every binary is registered the same way, so a case tagged [rt]
# anywhere is run that way.
function(clockwork_discover_tests target)
    catch_discover_tests(${target} TEST_SPEC "~[rt]" PROPERTIES SKIP_RETURN_CODE 4)
    catch_discover_tests(${target} TEST_SPEC "[rt]"
                         PROPERTIES LABELS rt RESOURCE_LOCK realtime SKIP_RETURN_CODE 4)
endfunction()
clockwork_discover_tests(clockwork_tests)

# ─── Plugin binaries ─────────────────────────────────────────────────────────
# A plugin binary built on Clockwork (clockwork_plugin_module,
# src/clockwork_plugin.h), and a host that loads two copies of it into one
# process (docs/ENGINES.md). The host links no Clockwork of its own, and
# so takes Catch2's own main rather than test_main.cpp, which does.
if(CLOCKWORK_PLUGIN_MODULES)
    add_library(clockwork_test_plugin MODULE plugin_module.cpp)
    clockwork_plugin_module(clockwork_test_plugin ENTRY clockwork_test_plugin_entry)
    add_executable(clockwork_plugin_module_tests test_plugin_module.cpp)
    target_link_libraries(clockwork_plugin_module_tests PRIVATE Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
    target_compile_definitions(clockwork_plugin_module_tests PRIVATE
        CLOCKWORK_TEST_PLUGIN="$<TARGET_FILE:clockwork_test_plugin>")
    add_dependencies(clockwork_plugin_module_tests clockwork_test_plugin)
    catch_discover_tests(clockwork_plugin_module_tests)
endif()

# ─── Hosts that make the core ────────────────────────────────────────────────
# Cases whose host makes the engine's core and closes it: the lanes ABI's
# lifecycle, the embed door, a core's close under other threads. Their own
# executable, so the cores they make and close are all the process holds:
# clockwork_tests' fixture holds one for the whole binary.
add_executable(clockwork_attach_tests
    test_main.cpp
    OscTestUtils.cpp
    test_attach.cpp
    test_core_close.cpp
    test_core_gate.cpp
    test_core_profile.cpp
    test_embed.cpp
    test_restart_grid_embed.cpp
)
target_include_directories(clockwork_attach_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_attach_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_attach_tests PRIVATE ${CLOCKWORK_LIB_GROUP} Catch2::Catch2)
target_compile_definitions(clockwork_attach_tests PRIVATE
    JUCE_APPLICATION_NAME_STRING="clockwork-attach-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
clockwork_discover_tests(clockwork_attach_tests)

# ─── A guest that leaves its instance count unset ───────────────────────────
# The dummy built to leave DspInfo::instances unset, as a zeroed DspInfo
# does: a guest a process holds once. Its own executable, because a process
# links one guest.
add_library(clockwork_dummy_unset STATIC ${PROJECT_SOURCE_DIR}/dsp/dummy/dummy_dsp.cpp)
target_link_libraries(clockwork_dummy_unset PRIVATE clockwork)
target_compile_definitions(clockwork_dummy_unset PRIVATE DUMMY_DSP_INSTANCES=0)
clockwork_lib_group(CLOCKWORK_UNSET_GROUP clockwork_dummy_unset)
add_executable(clockwork_guest_once_tests
    test_main.cpp
    test_guest_once.cpp
)
target_include_directories(clockwork_guest_once_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_guest_once_tests clockwork_dummy_unset)
target_link_libraries(clockwork_guest_once_tests PRIVATE ${CLOCKWORK_UNSET_GROUP} Catch2::Catch2)
target_compile_definitions(clockwork_guest_once_tests PRIVATE
    JUCE_APPLICATION_NAME_STRING="clockwork-guest-once-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
clockwork_discover_tests(clockwork_guest_once_tests)

# ─── Reference runs and independence ─────────────────────────────────────────
# What one engine does, recorded: each case a scenario whose trace must equal
# test/reference/<platform>/ (docs/ENGINES.md.1). Its own executable
# because each case needs an engine of its own, so ctest runs each in a
# process of its own. And the same scenarios beside each other, every core
# held to its trace alone (Part 4.2): each of those runs a whole engine per
# scenario at once, so it runs with no other case beside it.
string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" CLOCKWORK_REFERENCE_PLATFORM)
add_executable(clockwork_reference_tests
    test_main.cpp
    OscTestUtils.cpp
    LanesFixture.cpp
    test_reference.cpp
)
target_include_directories(clockwork_reference_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_reference_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_reference_tests PRIVATE ${CLOCKWORK_LIB_GROUP} Catch2::Catch2)
target_compile_definitions(clockwork_reference_tests PRIVATE
    CLOCKWORK_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/reference"
    CLOCKWORK_REFERENCE_PLATFORM="${CLOCKWORK_REFERENCE_PLATFORM}"
    JUCE_APPLICATION_NAME_STRING="clockwork-reference-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(clockwork_reference_tests PRIVATE /W1 /wd4244 /wd4267 /wd4305 /permissive-)
endif()
catch_discover_tests(clockwork_reference_tests TEST_SPEC "~[independence]" PROPERTIES SKIP_RETURN_CODE 4)
catch_discover_tests(clockwork_reference_tests TEST_SPEC "[independence]"
                     PROPERTIES RUN_SERIAL TRUE SKIP_RETURN_CODE 4)

# ─── Reserved lanes ──────────────────────────────────────────────────────────
# The channels above the device, where a Link peer's audio lands.
#
# Its own executable for the same reason the engine cases have one: clockwork
# is a process singleton, and reserving lanes changes the geometry for the
# whole process. Every case in clockwork_tests is written against
# kInChannels/kOutChannels, so a second geometry needs a second process rather
# than a boot flag the other cases would have to know about.
#
# 8 lanes for the host and 4 for Link Audio above them: enough for two stereo
# peers, and small enough that the widened staging buffers stay trivial.
add_executable(clockwork_lanes_reserved_tests
    test_main.cpp
    OscTestUtils.cpp
    LanesFixture.cpp
    test_reserved_lanes.cpp
)
target_include_directories(clockwork_lanes_reserved_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_lanes_reserved_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_lanes_reserved_tests PRIVATE ${CLOCKWORK_LIB_GROUP} Catch2::Catch2)
target_compile_definitions(clockwork_lanes_reserved_tests PRIVATE
    LANES_FIXTURE_RESERVED_LANES=8
    LANES_FIXTURE_LINK_AUDIO_LANES=4
    JUCE_APPLICATION_NAME_STRING="clockwork-lanes-reserved-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(clockwork_lanes_reserved_tests PRIVATE /W1 /wd4244 /wd4267 /wd4305 /permissive-)
endif()
clockwork_discover_tests(clockwork_lanes_reserved_tests)

# ─── The engine, booted whole ────────────────────────────────────────────────
# A ClockworkEngine and the lanes fixture both own the process-global
# audio_processor state, and the fixture never shuts down, so cases that boot
# an engine live in their own executable. No device: manualAudioPump +
# freewheelClock, the test renders the blocks; the [rt] cases boot the
# headless driver instead (clockwork_discover_tests above).
#
# The scheduled-out cases schedule something and pin what happens when the
# scheduler fires it, so the executable exists only when there is a
# scheduler to fire. Not a runtime skip, for the reason given above
# test_scheduler_absent.cpp: without a store the events never fire, the
# receives time out, and a "failure" would be reporting the build option.
# The engine-control cases ride along: they need only a booted engine, but
# not a second process of their own.
# A booted ClockworkEngine, so it needs the device layer as well as a scheduler.
if(CLOCKWORK_SCHEDULER AND CLOCKWORK_DEVICE)
add_executable(clockwork_engine_tests
    test_main.cpp
    OscTestUtils.cpp
    EngineFixture.cpp
    test_engine_control.cpp
    test_engine_lanes.cpp
    test_client_abi.cpp
    test_pumped_device.cpp
    test_embed_device.cpp
    test_client_rings.cpp
    test_smoothie_text.cpp
    # The engine as a whole: lifecycle, config, state machine, the control
    # plane, the watchdog, swaps and their recovery, device selection, the
    # no-device and fake-device boots, and the subsystems reached through
    # its ingress.
    test_engine_lifecycle.cpp
    test_log_hold.cpp
    test_config_validation.cpp
    test_engine_state.cpp
    test_params_engine.cpp
    test_restart_refusals.cpp
    test_restart_sequence.cpp
    test_restart_grid.cpp
    test_guest_reconfigure.cpp
    test_control_plane_liveness.cpp
    test_ingest_overload.cpp
    test_callback_watchdog.cpp
    test_headless_pacing.cpp
    test_audio_gap.cpp
    test_device_dropouts.cpp
    test_refused_in.cpp
    test_late_events.cpp
    test_cold_swap_recovery.cpp
    test_rate_skew_recovery.cpp
    test_driver_switch.cpp
    test_device_selection.cpp
    test_no_audio_device.cpp
    test_input_fallback.cpp
    test_boot_input_pairing.cpp
    # Every driver dates the Link Audio reads it makes: the device callback,
    # the headless thread, the manual pump. Compiles to nothing without Link Audio.
    test_link_audio_drivers.cpp
    # A Link peer's audio, heard exactly: a stand-in peer, the endpoint run by
    # hand, the engine pumped on a freewheel clock. Compiles to nothing
    # without Link Audio.
    test_link_audio_stand_in.cpp
    test_swap_characterization.cpp
    test_dead_device.cpp
    test_real_devices.cpp
    test_system_default.cpp
    test_device_switch_verb.cpp
    test_device_events.cpp
    test_device_names.cpp
    test_device_drivers.cpp
    test_device_report.cpp
    test_device_session.cpp
    test_device_intent.cpp
    test_restart_fade.cpp
    test_gateway_swap_gate.cpp
    test_engine_fixture.cpp
    test_guest_config_layout.cpp
    test_in_ring_drain.cpp
    test_shm_repro.cpp
    test_guest_schedule_flush.cpp
    test_metrics_shm.cpp
    test_tap_stream_reader.cpp
    test_shm_peer_plane.cpp
    test_shm_attach.cpp
    test_client_egress.cpp
    test_host_control.cpp
    test_master_tap.cpp
    test_audio_resume.cpp
    test_assets.cpp
    test_shm_peer_crash.cpp
    test_thread_lifetime.cpp
    test_gamepad.cpp
    test_midi.cpp
    test_osc.cpp
    test_verb_refusals.cpp
    test_clock_snapshot.cpp
)

# ─── The client half of the verb surface, or its absence ─────────────────────
# CLOCKWORK_CLIENT_VERBS (docs/SURFACE.md): midi/out/*, midi/clock/beat and
# osc/send are answered, or compiled out and refused by name. As with the
# timed store, each configuration gets the suite that applies to it and
# neither is skipped at runtime. The scheduled-out cases send only client
# verbs (a timed note, a timed OSC message), so they belong to the ON side.
if(CLOCKWORK_CLIENT_VERBS)
    target_sources(clockwork_engine_tests PRIVATE
        test_client_verbs.cpp
        test_scheduled_out.cpp)
else()
    target_sources(clockwork_engine_tests PRIVATE test_client_verbs_absent.cpp)
endif()

# An out-of-process peer for the shm command plane: test_shm_peer_crash.cpp
# forks it and SIGKILLs it mid-write. Built with clockwork's compile profile so
# the segment's layout guard accepts it at attach.
add_executable(clockwork_test_shm_peer peer_shm/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/workers/wait_word.cpp)
target_include_directories(clockwork_test_shm_peer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set_target_properties(clockwork_test_shm_peer PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(clockwork_test_shm_peer PRIVATE ${CLOCKWORK_COMPILE_DEFS})
find_package(Threads REQUIRED)
target_link_libraries(clockwork_test_shm_peer PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(clockwork_test_shm_peer PRIVATE rt)
elseif(WIN32)
    target_link_libraries(clockwork_test_shm_peer PRIVATE Synchronization)
endif()
target_compile_definitions(clockwork_engine_tests PRIVATE
    CLOCKWORK_TEST_SHM_PEER_BINARY="$<TARGET_FILE:clockwork_test_shm_peer>")
add_dependencies(clockwork_engine_tests clockwork_test_shm_peer)
# The MIDI clock-out follower verbs live in MidiControl, which exists only
# with the MIDI subsystem; the case drives them through the booted engine and
# reads what left off the engine's own MidiClockOut.
if(CLOCKWORK_MIDI)
    target_sources(clockwork_engine_tests PRIVATE test_midi_clock_follow.cpp)
endif()
target_include_directories(clockwork_engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_engine_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_engine_tests PRIVATE ${CLOCKWORK_LIB_GROUP} clockwork_comms Catch2::Catch2)
target_compile_definitions(clockwork_engine_tests PRIVATE
    JUCE_APPLICATION_NAME_STRING="clockwork-engine-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(clockwork_engine_tests PRIVATE ws2_32)
endif()
# A live track's tap, claimed in the engine's arena: the engine's half of
# the tracks (TrackControl), which exists only with the plugin host.
if(CLOCKWORK_PLUGINS)
    target_sources(clockwork_engine_tests PRIVATE test_track_tap.cpp)
endif()
clockwork_discover_tests(clockwork_engine_tests)
if(APPLE)
    # The MIDI case receives on a CoreMIDI virtual destination of its own.
    target_link_libraries(clockwork_engine_tests PRIVATE "-framework CoreMIDI")
endif()
endif()

# The tests are ours too: with CLOCKWORK_WERROR (the top-level CMakeLists.txt) a warning in one fails the build.
if(CLOCKWORK_WERROR AND NOT MSVC)
    foreach(_t clockwork_tests clockwork_attach_tests clockwork_reference_tests clockwork_lanes_reserved_tests clockwork_engine_tests clockwork_test_shm_peer)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE -Werror)
        endif()
    endforeach()
endif()
