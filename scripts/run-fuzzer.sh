#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# Runs a fuzzer, and keeps the inputs it finds out of the job's log.
#
# A crasher travels sealed (encrypt-crasher.sh), but libFuzzer prints an input
# that crashed, leaked or timed out into its own output whenever it is 256
# bytes or less: as hex and as text after the "base unit:" line, and as
# Base64. cargo-fuzz prints it again, as Rust's Debug of it. A public
# repository's job log is public. So the fuzzer's whole output is written to
# <artifacts>/fuzzer.log, which is sealed with the inputs, and the job's log
# gets the same lines with the inputs left out.
#
# Then the log is searched for every input written, as libFuzzer and
# cargo-fuzz print one, and the step fails if any got through: a new output
# format would otherwise leak in silence.
#
#   scripts/run-fuzzer.sh <artifacts> <fuzzer> [arguments...]
#
# Exits as the fuzzer did, or 3 when an input reached the log.
set -uo pipefail

artifacts="$1"; shift
mkdir -p "$artifacts"
raw="$artifacts/fuzzer.log"
shown="$(mktemp)"
trap 'rm -f "$shown"' EXIT

"$@" 2>&1 | tee "$raw" | awk '
    # libFuzzer: the mutation line ends "; base unit: <sha1>", then the input
    # as hex and as text when it is short, then the line saying where it was
    # written; and the input as Base64.
    /; base unit: / { print; fflush(); held = 1; next }
    held && /Test unit written to/ { held = 0; print; fflush(); next }
    held { next }
    /^Base64: / { print "Base64: (sealed with the input)"; fflush(); next }
    # cargo-fuzz: the input as Rust prints it, up to how to reproduce it.
    /^Output of `std::fmt::Debug`:/ { print "Output of `std::fmt::Debug`: (sealed with the input)"; fflush(); debug = 1; next }
    debug && /^Reproduce with:/ { debug = 0; print; fflush(); next }
    debug { next }
    { print; fflush() }
' | tee "$shown"
status=${PIPESTATUS[0]}

# Every input written, in each form it would be printed in: Base64; libFuzzer's
# hex ("0x0,0x2c,"); and Rust's Debug of a byte slice ("[0, 44").
checked=0 leaked=0
for f in "$artifacts"/*; do
    [ -f "$f" ] && [ -s "$f" ] && [ "$f" != "$raw" ] || continue
    checked=$((checked + 1))
    b64=$(base64 < "$f" | tr -d '\n')
    hex=$(od -An -v -tu1 "$f" | awk '{ for (i = 1; i <= NF; i++) printf "0x%x,", $i }')
    dbg=$(od -An -v -tu1 "$f" | awk '{ for (i = 1; i <= NF; i++) printf "%s%d", (n++ ? ", " : "["), $i }')
    for form in "Base64:$b64" "hex:$hex" "Debug:$dbg"; do
        if grep -qF -- "${form#*:}" "$shown"; then
            echo "run-fuzzer: $(basename "$f") reached the log as ${form%%:*}" >&2
            leaked=1
        fi
    done
done
echo "run-fuzzer: the log searched for each of the $checked input(s) written: $([ $leaked = 0 ] && echo 'none in it' || echo 'FOUND')"
[ $leaked = 0 ] || exit 3
exit "$status"
