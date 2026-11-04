#!/usr/bin/env bash
# No panic crosses into a host: every extern "C" function in the Rust
# sources is the fence alone, no_unwind(fallback, || ...) (clockwork-fence;
# docs/ENGINES.md). Parsed from the sources by clockwork-guards; the
# canary (scripts/ffi-canary.rs) must be caught.
set -uo pipefail
cd "$(dirname "$0")/.."
roots=()
for d in rust/*/src; do roots+=("$d"); done
exec cargo run --quiet --manifest-path rust/Cargo.toml -p clockwork-guards --bin check-ffi -- \
    --canary scripts/ffi-canary.rs "${roots[@]}"
