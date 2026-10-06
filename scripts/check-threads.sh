#!/usr/bin/env bash
# Engine-side native code owns its threads with std::jthread, never std::thread,
# and never detaches one.
#
# A std::thread member reaching its destructor still joinable calls
# std::terminate: an owner torn down on an early return, an exception, or a
# forgotten stop() ends the process. A detached thread outlives its owner and
# runs against freed state. std::jthread joins in its destructor and carries a
# std::stop_token, so stopping is request_stop() and a loop that reads the
# token; every loop registers a std::stop_callback that does its own wake
# (a condition variable, an atomic wait word, a doorbell), so request_stop()
# alone always suffices. One shape, no copies.
#
# std::thread::id and std::this_thread are not thread ownership and pass. A
# line that must own a std::thread for a reason (the NIF's lifecycle worker,
# leaked on purpose because the BEAM may halt without on_unload) carries a
# "thread-guard: allow" comment naming it.
set -uo pipefail
cd "$(dirname "$0")/.."

hits=$(grep -RnE '\bstd::thread\b|\.detach\(\)' \
    src \
    --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.mm' \
    --exclude-dir=vendor \
    | grep -vE 'std::thread::id|std::this_thread' \
    | grep -v 'thread-guard: allow' || true)

if [ -n "$hits" ]; then
    echo "std::thread or detach() in engine-side code (use std::jthread + std::stop_token; see scripts/check-threads.sh):"
    echo "$hits"
    exit 1
fi
echo "thread guard: clean"
