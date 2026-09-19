#!/usr/bin/env bash
# Run the test binaries given as arguments, and if one stops making progress,
# print what every thread in it was doing before the job is killed.
#
# A hung test otherwise reaches us as a step timeout and nothing else: on
# 2026-09-19 the engine suite stopped inside one case on linux-arm64 (the
# first such hang in 25 runs) and the log ended there, with no way to tell
# which lock it was on. --durations names the case; this names the threads.
#
# Nothing is installed and nothing is slowed down while the tests behave:
# gdb is fetched only once a binary has overrun, which is also why this is
# Linux-only — it is where apt makes that free.
set -uo pipefail

# Per binary, not per suite. The whole native step is capped at 15 minutes in
# ci.yml; the longest of these runs about two.
LIMIT_SECONDS=${CLOCKWORK_TEST_TIMEOUT:-360}

dump_stacks() {
    local pid=$1 name=$2
    echo "::error::${name} made no progress for ${LIMIT_SECONDS}s — dumping thread stacks"
    sudo apt-get install -y -qq gdb >/dev/null 2>&1 || {
        echo "gdb could not be installed; no stacks available"
        return
    }
    # Attaching to a process we did not fork needs this on the runner image.
    sudo sysctl -q -w kernel.yama.ptrace_scope=0 2>/dev/null || true
    sudo gdb -p "$pid" -batch \
        -ex 'set pagination off' \
        -ex 'thread apply all bt' 2>&1 || echo "gdb could not attach to ${pid}"
}

for t in "$@"; do
    echo "=== $t"
    # Its own process group where the runner has setsid (it does), so a hung
    # binary's children are killed with it rather than left behind.
    if command -v setsid >/dev/null 2>&1; then
        setsid "$t" --durations yes &
    else
        "$t" --durations yes &
    fi
    pid=$!

    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$LIMIT_SECONDS" ]; do
        sleep 5
        waited=$((waited + 5))
    done

    if kill -0 "$pid" 2>/dev/null; then
        dump_stacks "$pid" "$t"
        kill -9 -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        exit 1
    fi

    # Plain assignment, not `if ! wait`: the negation would make $? zero and a
    # failing suite would leave here green.
    wait "$pid"
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "::error::${t} failed (exit ${status})"
        exit "$status"
    fi
done
