#!/usr/bin/env bash
# Compile Smoothie's drivers for the targets the main build does not cover:
# iOS (device and simulator) on macOS; on Linux, Android when an NDK is at
# ANDROID_NDK_ROOT or ANDROID_NDK_LATEST_HOME, else Bela against
# check/bela/Bela.h. Build only.
#
#   scripts/check-drivers.sh [build-dir]
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build-drivers}"
ran=0

build() {
    local name="$1"; shift
    echo "== $name"
    rm -rf "$out/$name"
    cmake -S smoothie -B "$out/$name" -Wno-dev "$@" >"$out/$name.configure.log" 2>&1 \
        || { cat "$out/$name.configure.log"; echo "check-drivers: $name failed to configure"; exit 1; }
    cmake --build "$out/$name" --parallel 4 \
        || { echo "check-drivers: $name failed to build"; exit 1; }
    ran=$((ran + 1))
}

mkdir -p "$out"
case "$(uname -s)" in
    Darwin)
        build ios-device    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos        -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
        build ios-simulator -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
        ;;
    Linux)
        ndk="${ANDROID_NDK_ROOT:-${ANDROID_NDK_LATEST_HOME:-}}"
        if [ -n "$ndk" ] && [ -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
            build android -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
        else
            echo "== android: no NDK (ANDROID_NDK_ROOT / ANDROID_NDK_LATEST_HOME), skipped"
            build bela -DSMOOTHIE_BELA=ON "-DCMAKE_CXX_FLAGS=-I$PWD/smoothie/check/bela"
        fi
        ;;
esac

echo "check-drivers: $ran target(s) built"
test "$ran" -gt 0
