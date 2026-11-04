#!/usr/bin/env bash
# A core's regions are where its layout puts them (shared_memory.h,
# clockwork::Layout, docs/ENGINES.md), so engine code reaches them
# through the core's layout and never through the target's default: a
# region found by the default lies elsewhere in a core with a profile of
# its own.
#
# The default profile's knobs (memory_profile.h) and kDefaultProfile /
# kDefaultLayout are named only where the default is made (memory_profile.h,
# shared_memory.h) and on lines carrying a "default-layout-ok:" comment that
# says why: memory that exists before any core, or a core's starting layout.
#
# Proves it looked: the canary (scripts/layout-canary.cpp) names the default
# where it must not, and the guard must catch it; the count of files read is
# printed with the result.
set -uo pipefail
cd "$(dirname "$0")/.."

names='\b(kDefaultLayout|kDefaultProfile|TAPS_SLOTS|TAP_TRACK_SLOTS|TAP_RING_FRAMES|CLOCKWORK_IN_RING_SIZE|CLOCKWORK_RT_OUT_RING_SIZE|CLOCKWORK_NRT_OUT_RING_SIZE|CLOCKWORK_SCOPE_FRAMES|CLOCKWORK_TAP_CHANNELS|CLOCKWORK_WINDOW_BYTES|GUEST_PERSIST_BYTES)\b'

scan() {
    grep -nE "$names" "$@" /dev/null \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' \
        | grep -v 'default-layout-ok:' || true
}

files=()
while IFS= read -r f; do files+=("$f"); done < <(
    find src -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.mm' \) \
        ! -path 'src/memory_profile.h' ! -path 'src/shared_memory.h' ! -path 'src/vendor/*' | sort)

if [ -z "$(scan scripts/layout-canary.cpp)" ]; then
    echo "check-layout: the canary was not caught: the guard is looking at nothing"
    exit 1
fi

hits=$(scan "${files[@]}")
if [ -n "$hits" ]; then
    echo "Engine code names the target's default layout or its knobs."
    echo "Reach a core's regions through its layout (clockwork::layout_of(core),"
    echo "CoreState::arena.layout); a use that is truly the default's carries a"
    echo "'default-layout-ok: <why>' comment."
    echo "$hits"
    exit 1
fi
allowed=$(grep -l 'default-layout-ok:' "${files[@]}" | wc -l | tr -d ' ')
echo "check-layout: read ${#files[@]} engine files; the default layout named only where it is made and in ${allowed} file(s) that say why; canary caught"
