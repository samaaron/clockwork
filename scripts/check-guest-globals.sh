#!/usr/bin/env bash
# A guest that says any number of it can run in a process
# (DspInfo::instances = DSP_INSTANCES_ANY) keeps nothing process-wide
# (docs/BOUNDARY.md, "How many in a process"): no mutable variable with
# static storage in its code — a global, a file or function-local static, a
# thread-local. This reads the guest's built C and C++ objects and fails on
# any.
#
# Build them unoptimised: an optimised build merges file statics into one
# block with no names. The canary (scripts/globals-canary.cpp) is a variable
# compiled the same way, which a scan that reads what it should reports.
#
#   scripts/check-guest-globals.sh <archive-or-object>...
#   CXX=<compiler>  the guest's compiler, for the canary (default c++)
set -euo pipefail
clockwork="$(cd "$(dirname "$0")/.." && pwd)"
if [ $# -eq 0 ]; then
    echo "usage: $0 <archive-or-object>..." >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
"${CXX:-c++}" -std=c++20 -O0 -c "$clockwork/scripts/globals-canary.cpp" -o "$work/globals-canary.o"
cargo run --quiet --manifest-path "$clockwork/rust/Cargo.toml" -p clockwork-guards --bin check-globals -- \
    --guest --canary "$work/globals-canary.o" "$@"
