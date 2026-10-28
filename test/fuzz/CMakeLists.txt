# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# Fuzz targets for the C and C++ code that reads untrusted bytes: oscpack's
# reader (every message a client sends the engine) and the standalone host's
# own reader. The audio decoders are Rust and fuzzed from rust/fuzz. A build of
# its own, kept out of the main tree's targets, so nothing here can change
# what clockwork ships.
#
#   With clang, each target is a libFuzzer binary:
#     CC=clang CXX=clang++ cmake -S test/fuzz -B build-fuzz && cmake --build build-fuzz
#     build-fuzz/fuzz_osc_received -max_total_time=300 corpus/ rust/clockwork-osc/corpus/seeds
#
#   With any other compiler, each target replays the files it is given
#   (replay_main.cpp), with ASan and UBSan where the compiler has them:
#     build-fuzz/fuzz_osc_received rust/clockwork-osc/corpus test/fuzz/corpus/osc_received

cmake_minimum_required(VERSION 3.20)
project(clockwork_fuzz C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CLOCKWORK_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CLOCKWORK_FUZZ_LIBFUZZER ON)
    set(CLOCKWORK_FUZZ_FLAGS -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined -g -O1)
    set(CLOCKWORK_FUZZ_LINK -fsanitize=fuzzer,address,undefined)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CLOCKWORK_FUZZ_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1)
    set(CLOCKWORK_FUZZ_LINK -fsanitize=address,undefined)
endif()

function(clockwork_fuzz_target name)
    add_executable(${name} ${ARGN})
    if(NOT CLOCKWORK_FUZZ_LIBFUZZER)
        target_sources(${name} PRIVATE replay_main.cpp)
    endif()
    target_include_directories(${name} PRIVATE ${CLOCKWORK_SRC} ${CLOCKWORK_SRC}/vendor ${CLOCKWORK_SRC}/vendor/oscpack)
    target_compile_options(${name} PRIVATE ${CLOCKWORK_FUZZ_FLAGS})
    target_link_options(${name} PRIVATE ${CLOCKWORK_FUZZ_LINK})
endfunction()

clockwork_fuzz_target(fuzz_osc_received fuzz_osc_received.cpp
    ${CLOCKWORK_SRC}/vendor/oscpack/osc/OscTypes.cpp
    ${CLOCKWORK_SRC}/vendor/oscpack/osc/OscReceivedElements.cpp)

clockwork_fuzz_target(fuzz_osc_reader fuzz_osc_reader.cpp)

# The audio writer: a WAV header and its patched sizes, and the FLAC encoder
# of flac_encoder.cpp. Its decoder-facing calls are stubbed (audio_decode_stubs.cpp)
# so the Rust workspace stays out of this build.
clockwork_fuzz_target(fuzz_audio_encode fuzz_audio_encode.cpp audio_decode_stubs.cpp
    ${CLOCKWORK_SRC}/clockwork_audio_file.cpp
    ${CLOCKWORK_SRC}/flac_encoder.cpp)
target_include_directories(fuzz_audio_encode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rust/clockwork-audio-decode/cpp)
