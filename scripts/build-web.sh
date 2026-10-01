#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2025-2026 Sam Aaron
#
# build-web.sh — clockwork compiled for the AudioWorklet.
#
# The web target is the same clockwork as the native one: the same C++ core, the
# same Rust subsystems, the same DSP boundary. What changes is which subsystems are
# present. midir, gilrs and std::net have no worklet to run in, so the umbrella
# crate is built with those features off — the SAME crate, not a web-only fork
# of it, which is why a subsystem cannot drift between the two targets.
#
# Output is a STANDALONE_WASM module with imported shared memory: the worklet
# supplies the memory, so JS and clockwork address one heap. The module is
# built by CMakeLists.txt under emcmake — the same tree as the native build —
# and this script wraps that and bundles the JavaScript around it.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC="$ROOT/src"
OUT="$ROOT/dist/wasm"

DSP="${CLOCKWORK_DSP:-dummy}"
SCHEDULER="${CLOCKWORK_SCHEDULER:-1}"
OPT="${CLOCKWORK_OPT:--O3}"

for arg in "$@"; do
    case "$arg" in
        --no-scheduler) SCHEDULER=0 ;;
        --dsp=*)        DSP="${arg#--dsp=}" ;;
        --debug)        OPT="-O0 -g" ;;
        *) echo "unknown argument: $arg" >&2; exit 1 ;;
    esac
done

command -v emcc >/dev/null || {
    echo "emcc not found. Run: source <emsdk>/emsdk_env.sh" >&2; exit 1; }

# emsdk ships its own node; prefer it so the build does not depend on a system
# node that may not exist (this box has none on PATH).
NODE="$(command -v node || ls -d "${EMSDK:-/nonexistent}"/node/*/bin/node 2>/dev/null | head -1)"
[ -x "$NODE" ] || { echo "no node found for the memory config reader" >&2; exit 1; }

mkdir -p "$OUT"

# ── The module: CMake, for the wasm target ───────────────────────────────────
# One tree, one list of sources, one set of flags: CMakeLists.txt configures
# the module when run under emcmake, reads the memory sizes from
# js/memory_layout.js, finds the nightly Rust the wasm target needs, and
# links the exports the worklet calls. This script wraps that and bundles the
# JavaScript around it.
BUILD_DIR="$ROOT/build/web"
CMAKE_BUILD_TYPE=Release
[ "$OPT" = "-O0 -g" ] && CMAKE_BUILD_TYPE=Debug
echo "clockwork → wasm   dsp=$DSP scheduler=$SCHEDULER ($CMAKE_BUILD_TYPE)"
emcc --version | head -1
emcmake cmake -B "$BUILD_DIR" -S "$ROOT" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCLOCKWORK_DSP="$DSP" \
    -DCLOCKWORK_SCHEDULER="$([ "$SCHEDULER" = 1 ] && echo ON || echo OFF)" \
    -DCLOCKWORK_WEB_AUDIO_FILES="$([ "${CLOCKWORK_WEB_AUDIO_FILES:-0}" = 1 ] && echo ON || echo OFF)" \
    ${CLOCKWORK_RUST_NIGHTLY:+-DCLOCKWORK_RUST_NIGHTLY="$CLOCKWORK_RUST_NIGHTLY"} \
    > "$BUILD_DIR.configure.log" 2>&1 || { cat "$BUILD_DIR.configure.log"; exit 1; }
cmake --build "$BUILD_DIR" --parallel

echo ""
echo "built $OUT/clockwork.wasm ($(du -h "$OUT/clockwork.wasm" | cut -f1))"

# ── The main-thread subsystems ───────────────────────────────────────────────
#
# MIDI and gamepad are not in the module above: Web MIDI and the Gamepad API
# exist on the main thread, so the client owns that I/O and drives the same
# Rust cores compiled to wasm-bindgen modules. Built BEFORE the bundle below,
# which imports their glue. See build-web-subsystems.sh.
#
# Optional. A product that wants neither (CLOCKWORK_WEB_SUBSYSTEMS=0), or a
# box without the pinned wasm-bindgen, still gets a client: the glue the
# managers import is replaced by a stub whose every function throws, so the
# bundle resolves, and a page that asks for `midi: true` is told at init
# that the subsystem was not built (it boots without it — see host_front.js).
if [ "${CLOCKWORK_WEB_SUBSYSTEMS:-1}" != "0" ] && "$SCRIPT_DIR/build-web-subsystems.sh"; then
    :
else
    echo "the MIDI and gamepad subsystems are not built: stubbing their glue so the client still bundles"
    for sub in midi gamepad; do
        mkdir -p "$ROOT/dist/$sub"
        "$NODE" - "$ROOT/js/lib/${sub}_manager.js" "$sub" > "$ROOT/dist/$sub/clockwork_${sub}.js" <<'JS'
const fs = require("node:fs");
const [, , manager, sub] = process.argv;
const src = fs.readFileSync(manager, "utf8");
const m = /import init,\s*\{([^}]*)\}\s*from\s*"[^"]*clockwork_[a-z]+\.js"/.exec(src);
const names = m[1].split(",").map((s) => s.trim()).filter(Boolean);
const why = `clockwork: the ${sub} subsystem was not built (build-web-subsystems.sh did not run — no wasm-bindgen?)`;
let out = `// A stub: the ${sub} subsystem was not built. Every call refuses.\n`;
out += `export default async function init() { throw new Error(${JSON.stringify(why)}); }\n`;
for (const n of names) {
  out += /^[A-Z]/.test(n)
    ? `export class ${n} { constructor() { throw new Error(${JSON.stringify(why)}); } }\n`
    : `export function ${n}() { throw new Error(${JSON.stringify(why)}); }\n`;
}
process.stdout.write(out);
JS
    done
fi

# ── The client and the workers ───────────────────────────────────────────────
#
# This step did not exist. build-web.sh produced a wasm module and nothing to
# drive it, so clockwork's own JS could not be loaded in a browser at all:
# growable_buffer_pool.js imports the bare specifier "@thi.ng/malloc", which
# no browser resolves without a bundler or an import map. Clockwork could
# therefore not construct its own client — which is why nothing it owned ever
# noticed that cell_pool.js had never been written.
#
# Skipped with a warning rather than failing: the wasm is useful on its own,
# and a box without node should still get one.
ESBUILD="${ESBUILD:-$ROOT/node_modules/.bin/esbuild}"
if [ -x "$ESBUILD" ]; then
    echo "bundling the client and workers..."
    DIST="$ROOT/dist"
    "$ESBUILD" "$ROOT/js/clockwork.js" --bundle --format=esm --define:__DEV__=true \
        --outfile="$DIST/clockwork.js" --external:./clockwork.wasm >/dev/null
    mkdir -p "$DIST/workers"
    for worker in "$ROOT/js/workers/"*.js; do
        "$ESBUILD" "$worker" --bundle --format=iife --define:__DEV__=true \
            --outfile="$DIST/workers/$(basename "$worker")" >/dev/null
    done
    echo "  -> dist/clockwork.js and dist/workers/"
else
    echo "  WARNING: no esbuild at $ESBUILD — skipping the JS bundle."
    echo "           npm install, then re-run, or the web client cannot load."
fi
