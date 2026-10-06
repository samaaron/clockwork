#!/bin/bash
# clang-tidy over Clockwork's own native sources (.clang-tidy says which
# checks), against the compile database of a build directory.
#
#   scripts/tidy.sh                 # every file, build/
#   scripts/tidy.sh build-link      # another build directory
#   scripts/tidy.sh build src/native/ClockworkEngine.cpp   # the files given
#
# The build directory needs a compile database:
#   cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
#
# On macOS Homebrew's clang-tidy has to be pointed at Apple's SDK and its
# libc++, or it analyses nothing: every standard header is "not found", and
# mixing Homebrew's libc++ with the SDK's C headers fails in <math.h>.
# Vendored code and smoothie are not ours to tidy and are left out.
set -eo pipefail
cd "$(dirname "$0")/.."

BUILD="${1:-build}"
shift || true
if [ ! -f "$BUILD/compile_commands.json" ]; then
    echo "no $BUILD/compile_commands.json: configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
    exit 2
fi

TIDY="${CLANG_TIDY:-$(command -v clang-tidy || true)}"
[ -z "$TIDY" ] && [ -x /opt/homebrew/opt/llvm/bin/clang-tidy ] && TIDY=/opt/homebrew/opt/llvm/bin/clang-tidy
[ -z "$TIDY" ] && { echo "clang-tidy not found (brew install llvm)" >&2; exit 2; }

EXTRA=()
if [ "$(uname)" = Darwin ]; then
    SDK="$(xcrun --show-sdk-path)"
    EXTRA=(--extra-arg=-isysroot --extra-arg="$SDK"
           --extra-arg=-stdlib=libc++ --extra-arg=-nostdinc++
           --extra-arg=-isystem --extra-arg="$SDK/usr/include/c++/v1")
fi

if [ $# -gt 0 ]; then
    FILES=("$@")
else
    FILES=($(ls src/*.cpp src/native/*.cpp src/clock/*.cpp src/comms/*.cpp src/lanes/*.cpp src/workers/*.cpp 2>/dev/null | grep -v vendor))
fi

"$TIDY" -p "$BUILD" --quiet "${EXTRA[@]}" "${FILES[@]}"
