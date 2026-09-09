#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# build-web-subsystems.sh — the MIDI and gamepad cores for the browser's main
# thread.
#
# The worklet cannot own a MIDI port or a game controller: Web MIDI and the
# Gamepad API exist on the main thread and nowhere else. So on the web those
# two subsystems live in the client (js/lib/midi_manager.js,
# js/lib/gamepad_manager.js), which does the I/O, and drive the SAME Rust
# crates the native engine links — clockwork-midi and clockwork-gamepad,
# compiled here to wasm-bindgen modules — for everything that is not I/O:
# parsing, encoding, name normalisation, the diffing, the payloads. One core,
# two hosts, so the /clockwork/midi/* and /clockwork/gamepad/* contracts
# cannot drift between them.
#
# Output: dist/midi/ and dist/gamepad/ (the JS glue the managers import) and
# the two *_bg.wasm files copied into dist/wasm/ beside the engine's module,
# which is where the client fetches wasm from (wasmBaseURL).
#
# Called by build-web.sh before the client is bundled; runnable on its own.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DIST="$ROOT/dist"
TARGET=wasm32-unknown-unknown

# The wasm-bindgen CLI must be the exact version of the wasm-bindgen crate in
# Cargo.lock: the glue it writes and the ABI the crate compiled are one
# contract. A project-local install is looked for first, so the version can
# be pinned here without touching whatever else is on the box:
#
#     cargo install wasm-bindgen-cli --version <lock version> --locked \
#         --root rust/target/tools
WANT="$(awk '/^name = "wasm-bindgen"$/{getline; sub(/version = /,""); gsub(/"/,""); print}' "$ROOT/rust/Cargo.lock")"
WASM_BINDGEN=""
for cand in "$ROOT/rust/target/tools/bin/wasm-bindgen" "$(command -v wasm-bindgen || true)"; do
    [ -x "$cand" ] || continue
    have="$("$cand" --version | awk '{print $2}')"
    if [ "$have" = "$WANT" ]; then WASM_BINDGEN="$cand"; break; fi
    echo "  (skipping $cand: wasm-bindgen $have, Cargo.lock wants $WANT)"
done
if [ -z "$WASM_BINDGEN" ]; then
    echo "no wasm-bindgen $WANT found. Install one beside the build:" >&2
    echo "  cargo install wasm-bindgen-cli --version $WANT --locked --root rust/target/tools" >&2
    echo "(with a toolchain new enough for its dependencies — the nightly the" >&2
    echo " web build already needs is; rustup run nightly cargo install ...)" >&2
    exit 1
fi

# The same nightly toolchain build-web.sh compiles the engine with, reached
# the same way (see there for why not `cargo +nightly`). Its rust-lld looks
# for libLLVM.dylib one directory below where rustup puts it, so the
# toolchain's lib/ is offered to the dynamic loader as a fallback.
if [ -z "${CARGO_NIGHTLY_BIN:-}" ] && command -v rustup >/dev/null; then
    CARGO_NIGHTLY_BIN="$(dirname "$(rustup which --toolchain nightly cargo 2>/dev/null)")" || true
fi
if [ -n "${CARGO_NIGHTLY_BIN:-}" ] && [ -x "$CARGO_NIGHTLY_BIN/cargo" ]; then
    PATH="$CARGO_NIGHTLY_BIN:$PATH"
    export DYLD_FALLBACK_LIBRARY_PATH="$(dirname "$CARGO_NIGHTLY_BIN")/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
else
    echo "no nightly Rust toolchain found. Run: rustup toolchain install nightly" >&2
    exit 1
fi

echo "building the main-thread subsystems for $TARGET..."
( cd "$ROOT/rust" && cargo build --release --target "$TARGET" -p clockwork-midi -p clockwork-gamepad )

mkdir -p "$DIST/wasm"
for crate in midi gamepad; do
    WASM="$ROOT/rust/target/$TARGET/release/clockwork_${crate}.wasm"
    [ -f "$WASM" ] || { echo "missing $WASM" >&2; exit 1; }
    "$WASM_BINDGEN" --target web --out-dir "$DIST/$crate" --out-name "clockwork_${crate}" "$WASM"
    # Beside the engine's module: the client fetches every wasm from one place.
    cp "$DIST/$crate/clockwork_${crate}_bg.wasm" "$DIST/wasm/"
    echo "  -> dist/$crate/ and dist/wasm/clockwork_${crate}_bg.wasm ($(du -h "$DIST/wasm/clockwork_${crate}_bg.wasm" | cut -f1))"
done
