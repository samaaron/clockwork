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

# A run that finds nothing must be shown to have looked. First the canary:
# two violations the check set has to report, through the same binary, SDK
# arguments and .clang-tidy as the real files. Nothing reported means
# clang-tidy is not analysing on this machine, and the run stops there
# rather than hand back an empty log that reads as clean.
CANARY=scripts/tidy-canary.cpp
CANARY_OUT="$("$TIDY" "${EXTRA[@]}" "$CANARY" -- -std=c++20 2>/dev/null || true)"
for check in cppcoreguidelines-no-malloc cppcoreguidelines-owning-memory; do
    if ! grep -q "\[$check[],]" <<< "$CANARY_OUT"; then   # "[check]", or "[check,-warnings-as-errors]" with the gate on
        echo "tidy: the canary ($CANARY) did not report $check — clang-tidy is not analysing" >&2
        echo "$CANARY_OUT" >&2
        exit 3
    fi
done

# Then the files. Progress goes to stderr as clang-tidy prints it; stdout is
# the diagnostics, followed by one line that says what the run covered, so
# the log is never empty and never ambiguous: how many files were analysed
# of how many asked for, how many warnings in Clockwork's own sources, and
# how many were suppressed in headers that are not ours (the proof that the
# checks ran over real code).
ERR="$(mktemp)"
trap 'rm -f "$ERR"' EXIT
exec 3>&1   # the diagnostics keep stdout; stderr is teed so progress still shows live
set +e
"$TIDY" -p "$BUILD" "${EXTRA[@]}" "${FILES[@]}" 2>&1 >&3 3>&- | tee "$ERR" >&2
STATUS=${PIPESTATUS[0]}
set -e
# clang-tidy names every file it could not analyse ("Error while processing
# X.": no such file, a compile error, a missing compile command); the rest
# were. The progress lines are not counted — a run of one file prints none.
# One file's compile error takes every file after it in the same invocation
# down with it (seen 2026-10-06: a Homebrew LLVM older than the SDK it was
# pointed at failed in <random>, and 41 of 50 files went unanalysed while
# the log showed one error), so the count below is the honest one and a
# single error is worth chasing before anything else.
FAILED=$(grep -c '^Error while processing' "$ERR" || true)
ANALYSED=$(( ${#FILES[@]} - FAILED ))
SUPPRESSED=$({ grep -o 'Suppressed [0-9]* warnings' "$ERR" || true; } | awk '{ s += $2 } END { print s + 0 }')
echo "tidy: ${ANALYSED}/${#FILES[@]} files analysed, canary reported, ${SUPPRESSED} warnings suppressed in headers that are not Clockwork's"
if [ "$FAILED" -ne 0 ]; then
    echo "tidy: ${FAILED} of ${#FILES[@]} files could not be analysed (see 'Error while processing' above)" >&2
    exit 4
fi
exit "$STATUS"
