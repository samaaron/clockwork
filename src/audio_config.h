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
 * All runtime code should read the actual block size from the core's
 * Build::block (core_state.h, exposed across the ABI as
 * clockwork_block_size(core)); these constants are the profile's limits a
 * build is held to.
 */

#include "memory_profile.h"

namespace clockwork {

#ifdef __EMSCRIPTEN__
// AudioWorklet render quantum is fixed at 128 samples by the Web Audio
// spec. Block size must equal render quantum exactly.
inline constexpr int kMaxBlockSize     = 128;
inline constexpr int kDefaultBlockSize = 128;
#else
// Non-WASM build — block size comes from memory_profile.h (default max 1024,
// which covers every HW buffer we've seen on macOS/Windows/Linux drivers,
// typically 64–512). Embedded profiles shrink this hard: a core's staging
// (core_state.h, Staging) holds a block of every channel each way, in fast
// RAM.
inline constexpr int kMaxBlockSize     = CLOCKWORK_MAX_BLOCK_SIZE;
inline constexpr int kDefaultBlockSize = CLOCKWORK_DEFAULT_BLOCK_SIZE;
#endif

// Per-DSP max audio channels, input and output alike (default 128); sized
// per device via memory_profile.h. Not platform-dependent.
inline constexpr int kMaxChannels = CLOCKWORK_MAX_CHANNELS;

} // namespace clockwork
