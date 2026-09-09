#!/bin/bash

# Build and test clockwork for 32-bit x86 (i686).
#
# Expects to already be running *inside* a 32-bit Debian userland — CI launches
# that container; see the native-linux-x86 job in .github/workflows/ci.yml.
#
# Why a container rather than multiarch on the normal runner: Ubuntu dropped
# i386 as a full architecture in 19.10, so the i386 -dev packages clockwork
# needs (ALSA, JACK) are not reliably there. Debian still carries a
# complete i386 archive. Nothing is emulated — x86_64 CPUs run 32-bit code
# natively, so this costs about what the x64 job does.
#
# 32-bit is worth covering because it is the one desktop target where the
# clockwork's lock-free assumptions can actually break: on i686, 8-byte atomics
# lower to cmpxchg8b and may need libatomic, where x86_64 gets them for free.
#
# Usage:
#   scripts/ci-linux-x86.sh [phase]
#
# Phases (default `all` runs them in order):
#   deps       apt dependencies + Rust toolchain
#   configure  cmake configure with tests on
#   build      clockwork, host binary and both test suites
#   test       both Catch2 suites
#   rust       cargo test for the Rust workspace, on i686

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/native-x86"

check_arch() {
    if [ "$(getconf LONG_BIT)" != "32" ]; then
        echo "ERROR: this script must run inside a 32-bit userland (getconf LONG_BIT = $(getconf LONG_BIT))." >&2
        exit 1
    fi
}

setup_paths() {
    if [ -f "${CARGO_HOME:-$HOME/.cargo}/env" ]; then
        # shellcheck disable=SC1091
        . "${CARGO_HOME:-$HOME/.cargo}/env"
    fi
    # CI runs the container as root against a workspace owned by the runner's
    # uid, which git otherwise refuses to touch. Scope it to the environment
    # rather than rewriting a global gitconfig.
    export GIT_CONFIG_COUNT=1
    export GIT_CONFIG_KEY_0=safe.directory
    export GIT_CONFIG_VALUE_0='*'
}

phase_deps() {
    echo "=== deps: apt + Rust ==="
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    # Mirrors the x64 job's dependency list. libjack-jackd2-dev rather than the
    # virtual libjack-dev: Debian has several providers, so apt can't resolve
    # the virtual name on its own.
    apt-get install -y \
        build-essential cmake pkg-config \
        ca-certificates curl git \
        libasound2-dev libudev-dev libjack-jackd2-dev

    # The Rust subsystems are cargo-built staticlibs the native build links.
    # i686-unknown-linux-gnu is a tier-1 target, but getting it installed takes
    # saying so: rustup-init infers the host triple from the KERNEL, which is
    # x86_64 even in a 32-bit userland, so left alone it installs a 64-bit
    # toolchain whose binaries cannot execute here. That install "succeeds"
    # (with a quiet "error reading rustc version") and the failure only
    # surfaces at build time as the shim reporting
    #   error: command failed: 'cargo': No such file or directory
    local triple=i686-unknown-linux-gnu
    setup_paths   # pick up a toolchain from an earlier run of this phase

    if ! cargo --version >/dev/null 2>&1; then
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
            | sh -s -- -y --profile minimal \
                --default-host "$triple" --default-toolchain stable
    fi
    setup_paths

    # Correct the toolchain only if it isn't already the right host — an image
    # carrying a pre-existing ~/.rustup keeps the default from its own
    # settings.toml and ignores the inferred one entirely.
    #
    # --force-non-host is required for the same reason the triple has to be
    # pinned: rustup reads the kernel (x86_64) and calls an i686 toolchain
    # foreign, refusing with "may not be able to run on this system". Nothing is
    # emulated here — 32-bit binaries run natively on this CPU.
    if ! rustc -vV 2>/dev/null | grep -q "^host: $triple$"; then
        rustup set default-host "$triple"
        rustup toolchain install --force-non-host --profile minimal "stable-$triple"
        rustup default --force-non-host "stable-$triple"
    fi

    # Fail here, with the toolchain in view, rather than 200 lines into a build.
    cargo --version
    rustc --version
}

phase_configure() {
    echo "=== configure ==="
    setup_paths
    cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
}

phase_build() {
    echo "=== build: clockwork + tests ==="
    setup_paths
    # Capped at 2 parallel jobs to match the x64 job: hosted runners run out
    # of memory above that on the test build.
    cmake --build "$BUILD_DIR" --config Release --parallel 2
}

phase_test() {
    echo "=== test: Catch2 suites ==="
    setup_paths
    CLOCKWORK_QUIET=1 "$BUILD_DIR/test/clockwork_tests"
    CLOCKWORK_QUIET=1 "$BUILD_DIR/test/clockwork_engine_tests"
}

phase_rust() {
    echo "=== rust: cargo test (i686) ==="
    setup_paths
    cargo test --release --manifest-path "$PROJECT_ROOT/rust/Cargo.toml"
}

main() {
    check_arch
    case "${1:-all}" in
        deps)      phase_deps ;;
        configure) phase_configure ;;
        build)     phase_build ;;
        test)      phase_test ;;
        rust)      phase_rust ;;
        all)
            phase_deps
            phase_configure
            phase_build
            phase_test
            phase_rust
            ;;
        *)
            echo "usage: $0 [deps|configure|build|test|rust|all]" >&2
            exit 2
            ;;
    esac
}

main "$@"
