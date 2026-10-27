#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# Real sample files the whole way round (test/test_samples_through_engine.cpp):
# every sample Sonic Pi ships, decoded by clockwork and by ffmpeg and compared,
# played through the pumped engine, written back out and read by both. Needs
# a built test binary, npm and ffmpeg. Run it before pushing a change to the
# decoders, the writers or the audio path; CI runs the same script (ci.yml,
# the native Linux job).
#
#   scripts/check-samples.sh                 # the published samples, cached under build/
#   scripts/check-samples.sh <dir>           # any directory of audio files
#   CLOCKWORK_BUILD=build-audit scripts/check-samples.sh   # another build directory
set -euo pipefail
cd "$(dirname "$0")/.."

SAMPLES_PACKAGE="supersonic-scsynth-samples"
SAMPLES_VERSION="0.92.0"   # pinned: a red run means clockwork changed, not the samples
BUILD="${CLOCKWORK_BUILD:-build}"
TESTS="$BUILD/test/clockwork_tests"

if [[ ! -x "$TESTS" ]]; then
    echo "check-samples: no test binary at $TESTS; build with -DBUILD_TESTS=ON first" >&2
    exit 2
fi

if [[ $# -ge 1 ]]; then
    dir="$1"
else
    cache="build/samples/$SAMPLES_PACKAGE-$SAMPLES_VERSION"
    if [[ ! -d "$cache/package/samples" ]]; then
        mkdir -p "$cache"
        (cd "$cache" && npm pack "$SAMPLES_PACKAGE@$SAMPLES_VERSION" --silent >/dev/null && tar xzf "$SAMPLES_PACKAGE-$SAMPLES_VERSION.tgz")
    fi
    dir="$cache/package/samples"
fi

echo "check-samples: $(ls "$dir" | wc -l | tr -d ' ') files in $dir"
CLOCKWORK_SAMPLES_DIR="$dir" "$TESTS" "[samples]"
