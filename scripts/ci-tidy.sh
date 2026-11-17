#!/usr/bin/env bash
# clang-tidy as CI runs it: the tidy job on Linux, and the native job's
# macOS arm64 row (.github/workflows/ci.yml). Runs scripts/tidy.sh over a
# build directory's compile database, writes tidy.log and tidy.err, puts the
# counts per check in the job summary, and fails only on what .clang-tidy
# makes an error.
#
#   scripts/ci-tidy.sh <build-dir> <platform>
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD="${1:?usage: ci-tidy.sh <build-dir> <platform>}"
PLATFORM="${2:?usage: ci-tidy.sh <build-dir> <platform>}"
SUMMARY="${GITHUB_STEP_SUMMARY:-/dev/stdout}"

scripts/tidy.sh "$BUILD" > tidy.log 2> tidy.err
rc=$?
{
    echo "## clang-tidy ($PLATFORM)"
    echo
    echo "| count | check |"
    echo "|------:|-------|"
    # grep exits 1 when there is nothing to find, which with pipefail would
    # fail the run for being at zero; an empty table is the aim.
    { grep -o '\[[a-z]*-[a-z0-9.-]*\]$' tidy.log || true; } | sort | uniq -c | sort -rn \
        | awk '{ printf "| %s | %s |\n", $1, $2 }'
    echo
    echo "$(grep -c 'warning:' tidy.log || true) warnings, $(grep -c 'clang-diagnostic-error' tidy.log || true) files not compiled by clang-tidy"
    echo
    # The script's own account of the run: files analysed of files asked
    # for, the canary, and what was suppressed outside our sources. An empty
    # table above is only clean if this line says every file was looked at.
    grep '^tidy:' tidy.log || echo "tidy.sh gave no account of the run (exit $rc)"
} >> "$SUMMARY"

# 2: no compile database or clang-tidy; 3: the canary reported nothing; 4:
# not every file was analysed. Each means the run is not evidence of
# anything, and must say so.
if [ "$rc" -ge 2 ]; then
    echo "tidy.sh exit $rc:"
    tail -20 tidy.err
    exit "$rc"
fi
# Only what .clang-tidy says is an error fails the run.
if grep -q '\[-warnings-as-errors\]' tidy.log; then
    grep -B1 '\[-warnings-as-errors\]' tidy.log | head -40
    exit 1
fi
exit 0
