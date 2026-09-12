#!/usr/bin/env bash
# Engine-side native code logs through clockwork_log, never through stdio.
#
# clockwork_log frames a line onto the debug ring, where a host prints it, a
# client shows it and the test fixture captures it. A raw fprintf(stderr)
# reaches none of those: it is invisible to a client, invisible to a test's
# failure dump, and on the web it goes nowhere at all. Lines that genuinely
# have no ring to go to (reporting a boot that produced no ring, or the hold
# fallback inside clockwork_log itself) carry a "stdio-ok:" comment naming
# why.
#
# Exempt: src/host (clockwork's own reference host — a process main, whose
# stderr is its own), PluginBridgeMain.cpp and PluginEditorWindow{Win.cpp,.mm} (the
# plugin bridge, a separate process with a stderr of its own and no engine
# ring to reach).
set -uo pipefail
cd "$(dirname "$0")/.."

hits=$(grep -RnE '^[^/]*(\b(printf|fprintf|vprintf|vfprintf|puts|fputs|fputc|putchar|perror)[[:space:]]*\(|std::(cerr|cout|clog)\b)' \
    src \
    --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.mm' \
    --exclude-dir=host \
    --exclude=PluginBridgeMain.cpp \
    --exclude=PluginEditorWindowWin.cpp \
    --exclude=PluginEditorWindow.mm \
    | grep -v 'stdio-ok:' || true)

# The Rust subsystems' equivalent: clockwork_log::log! (rust/clockwork-log),
# never println!/eprintln!. Build scripts speak to cargo through println and
# are exempt; so is the bridge (a process of its own) and a crate's tests.
rust_hits=$(grep -RnE '\b(eprintln|println|eprint|print)!\(' \
    rust --include='*.rs' \
    --exclude=build.rs \
    --exclude-dir=target --exclude-dir=tests --exclude-dir=examples --exclude-dir=benches \
    --exclude-dir=clockwork-bridge-native \
    | grep -v 'stdio-ok:' || true)
hits="${hits}${hits:+$'\n'}${rust_hits}"
hits="${hits#$'\n'}"

if [ -n "$hits" ]; then
    echo "Raw stdio logging found in engine-side native code."
    echo "Log through clockwork_log (see clockwork_config.h) or, in Rust,"
    echo "clockwork_log::log!; a line that truly has no ring to go to carries a"
    echo "'stdio-ok: <why>' comment."
    echo "$hits"
    exit 1
fi
echo "check-logging: no raw stdio in engine-side native code (C++ and Rust)"
