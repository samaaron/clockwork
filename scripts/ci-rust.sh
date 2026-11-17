#!/usr/bin/env bash
# The Rust workspace's checks as CI runs them: the rust job on Linux and
# Windows, and the native job's macOS rows (.github/workflows/ci.yml).
#
#   scripts/ci-rust.sh clippy   # every target, warnings as errors
#   scripts/ci-rust.sh test     # the tests, in release
#
# Warnings are errors in clippy so an unsafe block with no SAFETY comment, or
# any other lint rust/Cargo.toml's [workspace.lints] sets, cannot slip in.
# clockwork-client's tests link the engine through clockwork-sys, whose build
# script configures and builds the CMake tree into cargo's target directory,
# so both need what the C++ build needs.
set -euo pipefail
cd "$(dirname "$0")/.."

case "${1:-}" in
    clippy) cargo clippy --workspace --all-targets --manifest-path rust/Cargo.toml -- -D warnings ;;
    test)   cargo test --release --manifest-path rust/Cargo.toml ;;
    *)      echo "usage: $(basename "$0") clippy|test" >&2; exit 2 ;;
esac
