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

add_executable(clockwork_tests
    test_main.cpp

    # Shared fixtures.
    OscTestUtils.cpp

    # The rings and the lanes ABI — how bytes get in and out.
    test_ring_writer.cpp
    test_ring_buffer_write.cpp
    test_ring_reader.cpp
    test_ring_concurrency.cpp
    test_lanes.cpp
    test_arena.cpp
    test_audio_taps.cpp
    test_embed.cpp

    # OSC: the codec, the ingress router, and the one prefix clockwork keeps.
    test_osc_codec.cpp
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
    # whichever backing is compiled — the session-of-one or Link — because the
    # two disagreed and both were wrong.
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
    test_audio_block_clock.cpp
    test_time_source.cpp

    # The shared-memory segment: layout guard and permissions.
    test_shm_segment.cpp

    # The client's pool over the inbox lane.
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
    test_rt_alloc.cpp
    # The origin-token reply path, the ring wire against its golden corpus,
    # the shm segment as a trust boundary, the graph-robustness policy, and
    # the command transports against real sockets.
    test_reply_routing.cpp
    test_sink_profile.cpp
    test_ring_wire_conformance.cpp
    test_server_shm_security.cpp
    test_hardening_policy.cpp
    test_transports.cpp

    # The port substrate (rust/clockwork-ports) through its C ABI, its two
    # disk endpoints (rust/clockwork-ports-disk), and the boundary they meet the
    # DSP on. The ring's own arithmetic and its no-allocation rule are pinned
    # in Rust, where the allocator can actually be seen.
    test_ports.cpp

    # Reading and writing audio files (src/clockwork_audio_file.h): the
    # decoders, this repository's FLAC encoder, and the versioned structs
    # they fill. Needs no device and no engine.
    test_audio_file.cpp

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

    # Large payloads across the boundary, both ways — and the claim that there is
    # no separate door for them in either direction. Also fixes the capacity
    # asymmetry (ingress ring 6x the RT egress ring) as a measured fact.
    test_dsp_blobs.cpp

    # The guest to client audio-publishing region (shm_scope_stream.hpp): the
    # ring a waveform display reads. Header-only and lock-free, so both halves
    # of the contract are testable here without a device or a guest.
    test_scope_stream.cpp

    # The audio capture region (shm_audio_buffer.hpp): the rolling window a
    # recorder or a meter pulls from. Its real contract is what happens when
    # the reader falls behind, which is the case these pin hardest.
    test_shm_audio_buffer.cpp

    # MIDI clock out. NOT gated on CLOCKWORK_SCHEDULER: its whole point is
    # that 24 pulses a beat are ClockworkClock's business and arrive whether or not
    # this build compiled the timed queue. Gating it is how the gap it now
    # covers went unnoticed.
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
# skipped at runtime: a case that reports "passed" because it never ran is the
# thing this project keeps finding out the hard way.
if(CLOCKWORK_SCHEDULER)
    target_sources(clockwork_tests PRIVATE test_scheduler.cpp)
else()
    target_sources(clockwork_tests PRIVATE test_scheduler_absent.cpp)
endif()

# Device policy exists only when the device layer does: DevicePolicy is
# smoothie/JUCE. See CLOCKWORK_DEVICE.
if(CLOCKWORK_DEVICE)
    target_sources(clockwork_tests PRIVATE
        test_device_policy.cpp
        test_aggregate_buffer_clamp.cpp
        test_audio_recovery.cpp
        test_realtime_thread.cpp)
endif()

target_include_directories(clockwork_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_tests PRIVATE ${CLOCKWORK_LIB_GROUP} clockwork_comms Catch2::Catch2)
target_compile_definitions(clockwork_tests PRIVATE
    RING_WIRE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ring_wire.txt")

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
catch_discover_tests(clockwork_tests)

# ─── Attaching ───────────────────────────────────────────────────────────────
# More than one host in one process. Its own executable because it walks the
# process-wide attach count from zero, which no other binary can still be at.
add_executable(clockwork_attach_tests
    test_main.cpp
    test_attach.cpp
)
target_include_directories(clockwork_attach_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(clockwork_attach_tests ${CLOCKWORK_DSP})
target_link_libraries(clockwork_attach_tests PRIVATE ${CLOCKWORK_LIB_GROUP} Catch2::Catch2)
target_compile_definitions(clockwork_attach_tests PRIVATE
    JUCE_APPLICATION_NAME_STRING="clockwork-attach-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
catch_discover_tests(clockwork_attach_tests)

# ─── Reserved lanes ──────────────────────────────────────────────────────────
# The channels above the device, where a Link peer's audio lands.
#
# Its own executable for the same reason the engine cases have one: clockwork
# is a process singleton, and reserving lanes changes the geometry for the
# whole process. Every case in clockwork_tests is written against
# kInChannels/kOutChannels, so a second geometry needs a second process rather
# than a boot flag the other cases would have to know about.
#
# 8 lanes: enough for four stereo peers, and small enough that the widened
# staging buffers stay trivial.
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
    JUCE_APPLICATION_NAME_STRING="clockwork-lanes-reserved-tests"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(clockwork_lanes_reserved_tests PRIVATE /W1 /wd4244 /wd4267 /wd4305 /permissive-)
endif()
catch_discover_tests(clockwork_lanes_reserved_tests)

# ─── The engine, booted whole ────────────────────────────────────────────────
# A ClockworkEngine and the lanes fixture both own the process-global
# audio_processor state, and the fixture never shuts down, so cases that boot
# an engine live in their own executable. No device: manualAudioPump +
# freewheelClock, the test renders the blocks.
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
    test_embed_device.cpp
    test_client_rings.cpp
    # The engine as a whole: lifecycle, config, state machine, the control
    # plane, the watchdog, swaps and their recovery, device selection, the
    # no-device and fake-device boots, and the subsystems reached through
    # its ingress.
    test_engine_lifecycle.cpp
    test_config_validation.cpp
    test_engine_state.cpp
    test_control_plane_liveness.cpp
    test_callback_watchdog.cpp
    test_cold_swap_recovery.cpp
    test_driver_switch.cpp
    test_device_selection.cpp
    test_no_audio_device.cpp
    test_input_fallback.cpp
    test_boot_input_pairing.cpp
    test_swap_characterization.cpp
    test_guest_config_layout.cpp
    test_in_ring_drain.cpp
    test_shm_repro.cpp
    test_metrics_shm.cpp
    test_scope_stream_reader.cpp
    test_shm_peer_plane.cpp
    test_shm_attach.cpp
    test_client_egress.cpp
    test_host_control.cpp
    test_master_tap.cpp
    test_assets.cpp
    test_shm_peer_crash.cpp
    test_gamepad.cpp
    test_midi.cpp
    test_osc.cpp
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
add_executable(clockwork_test_shm_peer peer_shm/main.cpp)
target_include_directories(clockwork_test_shm_peer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set_target_properties(clockwork_test_shm_peer PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_definitions(clockwork_test_shm_peer PRIVATE ${CLOCKWORK_COMPILE_DEFS})
find_package(Threads REQUIRED)
target_link_libraries(clockwork_test_shm_peer PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(clockwork_test_shm_peer PRIVATE rt)
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
catch_discover_tests(clockwork_engine_tests)
if(APPLE)
    # The MIDI case receives on a CoreMIDI virtual destination of its own.
    target_link_libraries(clockwork_engine_tests PRIVATE "-framework CoreMIDI")
endif()
endif()
