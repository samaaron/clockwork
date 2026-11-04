#!/usr/bin/env bash
# Engine-side native code logs through clockwork_log, never through stdio,
# and every line names its core.
#
# clockwork_log frames a line onto its core's debug ring, where a host prints
# it, a client shows it and the test fixture captures it. A raw
# fprintf(stderr) reaches none of those: it is invisible to a client,
# invisible to a test's failure dump, and on the web it goes nowhere at all.
# Lines that genuinely have no ring to go to (reporting a boot that produced
# no ring, or the hold fallback inside clockwork_log itself) carry a
# "stdio-ok:" comment naming why.
#
# The core is the first argument, so the compiler holds every call to naming
# one; a call that names none on purpose (nullptr: a line of the process's,
# not any engine's) carries a "no-core:" comment naming why.
#
# Exempt: src/host (clockwork's own reference host — a process main, whose
# stderr is its own), PluginBridgeMain.cpp and PluginEditorWindow{Win.cpp,.mm} (the
# plugin bridge, a separate process with a stderr of its own and no engine
# ring to reach), and src/vendor/libsamplerate (kept byte for byte: of its
# two printfs one is inside a comment and the other under SRC_DEBUG, which
# src_linear.c sets to 0, so it never writes to stdio).
set -uo pipefail
cd "$(dirname "$0")/.."

hits=$(grep -RnE '^[^/]*(\b(printf|fprintf|vprintf|vfprintf|puts|fputs|fputc|putchar|perror)[[:space:]]*\(|std::(cerr|cout|clog)\b)' \
    src \
    --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.mm' \
    --exclude-dir=host \
    --exclude-dir=libsamplerate \
    --exclude=PluginBridgeMain.cpp \
    --exclude=PluginEditorWindowWin.cpp \
    --exclude=PluginEditorWindow.mm \
    | grep -v 'stdio-ok:' || true)

# The Rust subsystems' equivalent: clockwork_log::log! (rust/clockwork-log),
# never println!/eprintln!. Build scripts speak to cargo through println and
# are exempt; so are the bridge (a process of its own), the guards (a
# development tool whose report is its output) and a crate's tests.
rust_hits=$(grep -RnE '\b(eprintln|println|eprint|print)!\(' \
    rust --include='*.rs' \
    --exclude=build.rs \
    --exclude-dir=target --exclude-dir=tests --exclude-dir=examples --exclude-dir=benches \
    --exclude-dir=clockwork-bridge-native \
    --exclude-dir=clockwork-guards \
    | grep -v 'stdio-ok:' || true)
no_core=$(grep -RnE 'clockwork_log(_raw|_va)?\([[:space:]]*(nullptr|NULL|0)[[:space:]]*,' \
    src --include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.mm' \
    --exclude-dir=host \
    | grep -v 'no-core:' || true)
rust_no_core=$(grep -RnE 'log!\([[:space:]]*(std::ptr::null_mut\(\)|core::ptr::null_mut\(\)|ptr::null_mut\(\))[[:space:]]*,' \
    rust --include='*.rs' \
    --exclude-dir=target --exclude-dir=tests --exclude-dir=examples --exclude-dir=benches \
    | grep -v 'no-core:' || true)
hits="${hits}${hits:+$'\n'}${rust_hits}"
hits="${hits}${hits:+$'\n'}${no_core}"
hits="${hits}${hits:+$'\n'}${rust_no_core}"
hits="${hits#$'\n'}"

if [ -n "$hits" ]; then
    echo "Raw stdio logging, or a log line naming no core, in engine-side native code."
    echo "Log through clockwork_log(core, ...) (see clockwork_config.h) or, in Rust,"
    echo "clockwork_log::log!(core, ...); a line that truly has no ring to go to carries a"
    echo "'stdio-ok: <why>' comment, and one that names no core on purpose a 'no-core: <why>'."
    echo "$hits"
    exit 1
fi
echo "check-logging: no raw stdio in engine-side native code (C++ and Rust), every log line names its core"
