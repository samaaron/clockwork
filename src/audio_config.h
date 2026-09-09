// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
#pragma once
/*
 * audio_config.h
 *
 * Platform-specific audio configuration caps. Keeps the 128-sample block
 * size pin on the web build (AudioWorklet's fixed render quantum) while
 * letting native pick a block size that matches the hardware callback.
 *
 * All runtime code should read the actual block size from g_block_size
 * (audio_processor.cpp, exposed across the ABI as clockwork_block_size()); these
 * constants only size static buffers (like static_audio_bus) which need a
 * compile-time max.
 */

#include <cstdio>
#include <cstdlib>

#include "memory_profile.h"

namespace clockwork {

// Runtime-gated diagnostic logging, for detail that would pollute normal
// operation. Enabled for a specific bug report by launching with
// CLOCKWORK_DEV_LOG=1 in the environment — no rebuild needed.
//
// Evaluated once on first call and cached in a function-local static,
// so the hot-path cost is a single atomic load after initialisation.
// Compile-time gating would save even that but forces a rebuild for
// support requests, which is a worse tradeoff.
inline bool devLogEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("CLOCKWORK_DEV_LOG");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

#ifdef __EMSCRIPTEN__
// AudioWorklet render quantum is fixed at 128 samples by the Web Audio
// spec. Block size must equal render quantum exactly.
inline constexpr int kMaxBlockSize     = 128;
inline constexpr int kDefaultBlockSize = 128;
#else
// Non-WASM build — block size comes from memory_profile.h (default max 1024,
// which covers every HW buffer we've seen on macOS/Windows/Linux drivers,
// typically 64–512). Embedded profiles shrink this hard: static_audio_bus
// costs kMaxBlockSize * kMaxChannels * 4 B in .bss, so 1024 × 128 = 512 KB on
// desktop but e.g. 64 × 2 = 512 B on the ESP32-S3 profile.
inline constexpr int kMaxBlockSize     = CLOCKWORK_MAX_BLOCK_SIZE;
inline constexpr int kDefaultBlockSize = CLOCKWORK_DEFAULT_BLOCK_SIZE;
#endif

// Per-DSP max audio channels, input and output alike (default 128); sized
// per device via memory_profile.h. Not platform-dependent.
inline constexpr int kMaxChannels = CLOCKWORK_MAX_CHANNELS;

} // namespace clockwork

#define DEV_LOG(fmt, ...) \
    do { if (clockwork::devLogEnabled()) { \
        fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); \
    } } while (0)
