#!/usr/bin/env bash
# oscpack's fixed-size types (src/vendor/oscpack/osc/OscTypes.h) are the sizes
# the OSC wire format needs on every target the engine builds for. The header
# checks them with static_assert; this compiles it for each target, so the
# targets no CI job builds with are checked too: GCC on Windows (MinGW), where
# long is 32 bits, gave a 4-byte int64 (CLOCKWORK-CHANGES.md there, 5).
# clang, syntax only: the header needs nothing from any target's headers.
#
# Proves it looked: the canary asks for a 32-bit long on 64-bit MinGW, so a
# clang that ignored --target (and checked the host every time) fails it; the
# count of targets is printed with the result.
set -uo pipefail
cd "$(dirname "$0")/.."

CLANG="${CLANG:-clang++}"
command -v "$CLANG" >/dev/null || { echo "check-oscpack-types: no $CLANG"; exit 1; }
targets=(
    x86_64-pc-windows-msvc aarch64-pc-windows-msvc
    x86_64-w64-windows-gnu i686-w64-windows-gnu
    x86_64-linux-gnu aarch64-linux-gnu armv7-linux-gnueabihf i686-linux-gnu
    arm64-apple-macos x86_64-apple-macos arm64-apple-ios
    aarch64-linux-android armv7-linux-androideabi
    wasm32-unknown-emscripten
)

tu=$(mktemp "${TMPDIR:-/tmp}/oscpack-types.XXXXXX")
trap 'rm -f "$tu"' EXIT
if ! echo 'static_assert(sizeof(long) == 4, "canary");' \
    | "$CLANG" --target=x86_64-w64-windows-gnu -std=c++20 -fsyntax-only -x c++ - 2>/dev/null; then
    echo "check-oscpack-types: the canary failed: clang is not compiling for the targets asked"
    exit 1
fi

# the sizes asked here, not only the header's own static_asserts, so the guard holds whatever the header says
cat > "$tu" <<'CPP'
#include "osc/OscTypes.h"
static_assert(sizeof(osc::int32) == 4 && sizeof(osc::uint32) == 4, "osc::int32 is not 32 bits");
static_assert(sizeof(osc::int64) == 8 && sizeof(osc::uint64) == 8, "osc::int64 is not 64 bits");
CPP
failed=()
for t in "${targets[@]}"; do
    out=$("$CLANG" --target="$t" -std=c++20 -fsyntax-only -I src/vendor/oscpack -x c++ "$tu" 2>&1) \
        || { failed+=("$t"); echo "$t:"; echo "$out" | grep -E 'error' | head -3; }
done
if [ ${#failed[@]} -gt 0 ]; then
    echo "check-oscpack-types: osc's types are mis-sized on ${failed[*]}"
    exit 1
fi
echo "check-oscpack-types: osc::int32 and osc::int64 sized right on ${#targets[@]} targets; canary caught"
