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
# supplies the memory, so JS and clockwork address one heap.
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

echo "clockwork → wasm   dsp=$DSP scheduler=$SCHEDULER"
emcc --version | head -1

# Build-time and runtime memory come from ONE file. js/memory_layout.js is what
# the worklet allocates against; reading it here is what stops the module and
# its heap disagreeing about how big the heap is.
INITIAL_MEMORY=$("$NODE" "$SCRIPT_DIR/get_memory_config.js" initial)
MAX_MEMORY=$("$NODE" "$SCRIPT_DIR/get_memory_config.js" max)
# The placement arena the module claims once at init (audio_processor.cpp)
# when the host passes no size through clockwork_init. Same file, same reason.
CLOCKWORK_MEM_ARENA_SIZE=$("$NODE" "$SCRIPT_DIR/get_memory_config.js" arena)
echo "memory: $((INITIAL_MEMORY / 1024 / 1024))MB initial, $((MAX_MEMORY / 1024 / 1024))MB ceiling, $((CLOCKWORK_MEM_ARENA_SIZE / 1024 / 1024))MB arena"

SCHEDULER_DATA_POOL_SIZE=${SCHEDULER_DATA_POOL_SIZE:-$((4 * 1024 * 1024))}
SCHEDULER_SLOT_COUNT=${SCHEDULER_SLOT_COUNT:-8192}
CLOCKWORK_IN_BUFFER_SIZE=${CLOCKWORK_IN_BUFFER_SIZE:-$((1024 * 1024))}
WASM_STACK_SIZE=1048576

# ── Rust ─────────────────────────────────────────────────────────────────────
# One staticlib, because each carries its own std and two would collide at
# link time. `schedule` is the only feature the web build keeps: midi, gamepad
# and osc are native subsystems whose crates cannot run here.
#
# Shared memory needs the atomics and bulk-memory target features, and the
# precompiled std for this target has neither — hence -Z build-std, hence
# nightly. Without it the link fails with "--shared-memory is disallowed".
RUST_FEATURES=()
[ "$SCHEDULER" = "1" ] && RUST_FEATURES+=(schedule)
FEATURE_ARG=""
[ ${#RUST_FEATURES[@]} -gt 0 ] && FEATURE_ARG="--features $(IFS=,; echo "${RUST_FEATURES[*]}")"

# HOW NIGHTLY IS REACHED, and why not `cargo +nightly`.
#
# The `+toolchain` syntax is understood by rustup's cargo SHIM, not by cargo
# itself, so it fails wherever `cargo` on PATH is a real cargo rather than the
# shim. Homebrew's rust is exactly that: `brew install rustup` leaves a working
# nightly toolchain and a `cargo` that answers `+nightly` with "no such
# command", which reads like a missing toolchain and is not one.
#
# `rustup run nightly cargo` is not enough either, and fails FURTHER IN, which
# is worse: cargo is then nightly but it invokes whichever `rustc` is on PATH,
# so the stable one receives `-Z unstable-options` and reports "1 nightly
# option were parsed" from inside a build that had already compiled std.
#
# So put the whole toolchain's bin directory first and let cargo and rustc find
# each other there. Set CARGO_NIGHTLY_BIN to override.
# The pinned nightly first (CLOCKWORK_RUST_NIGHTLY; -Z build-std is unstable
# surface and a green build should not depend on the day someone last ran
# rustup update), then a floating `nightly`. Only a toolchain that is
# installed AND has rust-src is taken: asking rustup for one it lacks installs
# it bare, and build-std then fails several minutes in on the missing sources.
if [ -z "${CARGO_NIGHTLY_BIN:-}" ] && command -v rustup >/dev/null; then
    for tc in "${CLOCKWORK_RUST_NIGHTLY:-nightly-2026-07-02}" nightly; do
        rustup toolchain list 2>/dev/null | grep -q "^$tc" || continue
        rustc_path="$(rustup which --toolchain "$tc" rustc 2>/dev/null)" || continue
        [ -f "${rustc_path%/bin/rustc}/lib/rustlib/src/rust/library/Cargo.lock" ] || continue
        CARGO_NIGHTLY_BIN="$(dirname "$(rustup which --toolchain "$tc" cargo 2>/dev/null)")" && break
    done
fi
if [ -n "${CARGO_NIGHTLY_BIN:-}" ] && [ -x "$CARGO_NIGHTLY_BIN/cargo" ]; then
    PATH="$CARGO_NIGHTLY_BIN:$PATH"
else
    echo "no nightly Rust toolchain found. Run: rustup toolchain install nightly" >&2
    exit 1
fi

echo "building the Rust subsystems for wasm32-unknown-emscripten..."
( cd "$ROOT/rust" && \
  RUSTFLAGS="-C target-feature=+atomics,+bulk-memory,+mutable-globals" \
  cargo build --release -p clockwork-native \
      --target wasm32-unknown-emscripten \
      --no-default-features $FEATURE_ARG \
      -Z build-std=std,panic_abort )
RUST_LIB="$ROOT/rust/target/wasm32-unknown-emscripten/release/libclockwork_native.a"
[ -f "$RUST_LIB" ] || { echo "Rust staticlib missing: $RUST_LIB" >&2; exit 1; }

# ── C++ ──────────────────────────────────────────────────────────────────────
SOURCES=(
    "$SRC/audio_processor.cpp"
    # The client boundary. A browser client is a client like any other: it
    # opens by address rather than by segment, and otherwise runs the same
    # code a GUI in another process does.
    "$SRC/clockwork_client.cpp"
    "$SRC/lanes/lanes.cpp"
    "$SRC/clock/ClockworkClock.cpp"
    "$SRC/clock/EngineClock.cpp"
    "$SRC/scope_streams.cpp"
    "$SRC/engine_support.cpp"
    "$SRC/clockwork_heap.cpp"
    "$SRC/mem_region.cpp"
    "$SRC/clock/ClockworkClockNative.cpp"
    "$SRC/clock/TimeSource.cpp"
    "$SRC/clock/MidiTimelines.cpp"
    "$SRC/vendor/oscpack/osc/OscTypes.cpp"
    "$SRC/vendor/oscpack/osc/OscOutboundPacketStream.cpp"
    "$SRC/vendor/oscpack/osc/OscReceivedElements.cpp"
    "$SRC/clock/MidiClockOut.cpp"
    # Audio files (src/clockwork_audio_file.h). A browser tab decodes the same
    # formats a desktop does, from the same code — including AIFF, which the
    # platform's own decodeAudioData refuses.
    "$ROOT/dsp/$DSP/${DSP}_dsp.cpp"
)

# The audio file codecs (dr_wav, dr_flac, dr_mp3, stb_vorbis, the FLAC
# encoder) are a native concern: the sample loader and the recorder. On the
# web, samples arrive already decoded through the browser's own decoder and
# nothing records to a file, so nothing calls them — they were 200 kB of
# wasm nobody reached. Opt in with CLOCKWORK_WEB_AUDIO_FILES=1 for a build
# whose client decodes in the module.
AUDIO_FILE_EXPORTS=""
if [ "${CLOCKWORK_WEB_AUDIO_FILES:-0}" = "1" ]; then
    SOURCES+=("$SRC/clockwork_audio_file.cpp" "$SRC/flac_encoder.cpp" "$SRC/vendor/stb/stb_vorbis.c")
    AUDIO_FILE_EXPORTS=",'_clockwork_audio_probe_memory','_clockwork_audio_decode_memory',\
'_clockwork_audio_free','_clockwork_audio_duration',\
'_clockwork_audio_can_write','_clockwork_audio_writer_open_memory',\
'_clockwork_audio_writer_write','_clockwork_audio_writer_frames',\
'_clockwork_audio_writer_close'"
fi

INCLUDES=(-I"$ROOT" -I"$SRC" -I"$SRC/vendor/oscpack")
for d in "$ROOT"/rust/*/cpp; do [ -d "$d" ] && INCLUDES+=(-I"$d"); done

# Every worklet entry point. A name missing here is dead-stripped and the
# worklet fails at run time, not at build time — so this list has to be right.
EXPORTS="['___wasm_call_ctors','_clockwork_init','_get_ring_buffer_base',\
'_clockwork_tick','_process_audio','_get_audio_output_bus','_get_audio_input_bus',\
'_get_audio_num_output_buses','_get_audio_num_input_buses','_get_audio_buffer_samples',\
'_clockwork_clock_wasm_init','_set_time_offset','_get_time_offset',\
'_clockwork_log','_clockwork_log_va','_clockwork_log_raw',\
'_get_process_count','_get_messages_processed','_get_messages_dropped','_get_status_flags',\
'_clear_scheduler','_clockwork_host_forward','_clockwork_host_forwards','_malloc','_free',\
'_clockwork_client_abi_version','_clockwork_client_status_text',\
'_clockwork_client_open_memory','_clockwork_client_open_memory_in',\
'_clockwork_client_sizeof','_clockwork_client_close','_clockwork_client_info',\
'_clockwork_client_send','_clockwork_client_send_begin',\
'_clockwork_client_send_commit','_clockwork_client_send_abort',\
'_clockwork_client_poll','_clockwork_client_region',\
'_clockwork_client_metrics','_clockwork_client_clock','_clockwork_client_beat_at',\
'_clockwork_client_tap_sizeof','_clockwork_client_tap_open',\
'_clockwork_client_tap_open_in','_clockwork_client_tap_close',\
'_clockwork_client_tap_poll','_clockwork_client_tap_missed',\
'_clockwork_client_scope_open','_clockwork_client_scope_valid',\
'_clockwork_client_scope_audible_end','_clockwork_client_scope_read'"$AUDIO_FILE_EXPORTS"]"

echo "compiling and linking..."
emcc "${SOURCES[@]}" "${INCLUDES[@]}" "$RUST_LIB" \
    -DCLOCKWORK_SYNTH=1 -DCLOCKWORK_WORKLET_CLOCK=1 -DNDEBUG \
    -DCLOCKWORK_AUDIO_NO_STDIO=1 -DSTB_VORBIS_NO_STDIO \
    -DCLOCKWORK_SCHEDULER=$SCHEDULER \
    -DSCHEDULER_DATA_POOL_SIZE=$SCHEDULER_DATA_POOL_SIZE \
    -DSCHEDULER_SLOT_COUNT=$SCHEDULER_SLOT_COUNT \
    -DSCHEDULER_SHED_LATE_MS=1000 \
    -DCLOCKWORK_IN_BUFFER_SIZE=$CLOCKWORK_IN_BUFFER_SIZE \
    -DCLOCKWORK_MEM_ARENA_SIZE=$CLOCKWORK_MEM_ARENA_SIZE \
    -o "$OUT/clockwork.wasm" \
    -sSTANDALONE_WASM \
    -sNO_FILESYSTEM=1 \
    -sENVIRONMENT=worker \
    -pthread \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=$INITIAL_MEMORY \
    -sMAXIMUM_MEMORY=$MAX_MEMORY \
    -sSTACK_SIZE=$WASM_STACK_SIZE \
    -sEXPORTED_FUNCTIONS="$EXPORTS" \
    --no-entry \
    -Wl,--import-memory,--shared-memory,--allow-multiple-definition \
    -fcommon $OPT -msimd128 -flto -fwasm-exceptions \
    -fno-math-errno -fsigned-zeros -fno-associative-math \
    -Wno-nontrivial-memcall -Wno-extern-c-compat \
    -sERROR_ON_UNDEFINED_SYMBOLS=1

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
