#!/usr/bin/env bash
# Every mutable variable with static storage in Clockwork's own code — a
# global, a file or function-local static, a thread-local, in C, C++ or Rust
# — is on scripts/globals.list, which says what it is: one per engine, one
# per process and why, a constant, a log cap, test-only (docs/ENGINES.md,
# Part 4.3).
#
# It reads the built objects, not the sources, so it sees what the compiler
# kept. It builds them unoptimised: an optimised build merges file statics
# into one block with no names. A new variable fails the check until the
# list says what it is, and a listed one the build no longer has fails it
# until its line goes. The canary (scripts/globals-canary.cpp) is a variable
# the list does not name, compiled the same way: a scan that does not
# report it fails. The dummy guest is held to the guests' rule instead
# (scripts/check-guest-globals.sh): it keeps no variable at all.
#
#   scripts/check-globals.sh [build-dir]        default build-globals
set -euo pipefail
cd "$(dirname "$0")/.."
build=${1:-build-globals}

cmake -B "$build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF . > /dev/null
with=()
targets=(clockwork dummy clockwork_comms clockwork_rust)
if grep -q '^CLOCKWORK_PLUGINS:BOOL=ON' "$build/CMakeCache.txt"; then
    with+=(--with plugins)
    targets+=(clockwork_plugin_host clockwork_plugin_clap clockwork_plugin_vst3)
fi
cmake --build "$build" --parallel 3 --target "${targets[@]}"

cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$build/CMakeCache.txt")
"$cxx" -std=c++20 -O0 -c scripts/globals-canary.cpp -o "$build/globals-canary.o"

inputs=("$build/libclockwork.a" "$build/libclockwork_comms.a" rust/target/debug/libclockwork_native.a)
if [ ${#with[@]} -gt 0 ]; then
    inputs+=("$build/libclockwork_plugin_host.a" "$build/plugins/clap/libclockwork_plugin_clap.a"
             "$build/plugins/vst3/libclockwork_plugin_vst3.a")
fi
cargo run --quiet --manifest-path rust/Cargo.toml -p clockwork-guards --bin check-globals -- \
    --list scripts/globals.list --canary "$build/globals-canary.o" ${with[@]+"${with[@]}"} "${inputs[@]}"

# The dummy guest says any number of it can run in a process, and so keeps no
# variable at all.
CXX="$cxx" scripts/check-guest-globals.sh "$build/dsp-dummy/libdummy.a"
