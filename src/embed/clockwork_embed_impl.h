// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The handle behind clockwork_embed.h, shared by the attach path
// (clockwork_embed.cpp) and the device boot (native/ClockworkEmbedDevice.cpp).
#pragma once
#include "clockwork_embed.h"
#include <atomic>
#include <cstdint>
#include <vector>

struct ClockworkEmbed {
    enum class Kind { Booted, Attached } kind = Kind::Attached;
    ClockworkClient* client = nullptr;
    bool ownsClient = false;        // attach opens one over the arena; boot borrows the engine's
    double   sampleRate = 0;
    uint32_t block = 0;
    uint32_t inChannels = 0, outChannels = 0;

    // The device host: the engine and how to close it, from the device TU.
    void* engine = nullptr;
    void (*closeEngine)(void*) = nullptr;
    ClockworkStatus (*deviceInfo)(void*, ClockworkEmbedDevice*) = nullptr;

    // The rendering host: two FIFOs, each `channels` rings of `capacity`
    // frames, and a clock. Sized at attach; never touched by an allocator
    // after.
    uint32_t capacity = 0;
    std::vector<float> outRing, inRing;
    uint32_t outHead = 0, outCount = 0;   // rendered, not yet served
    uint32_t inHead = 0,  inCount = 0;    // received, not yet ticked
    double   ntpBase = 0;
    uint64_t rendered = 0;

    // Owned by this handle; the single attachment it holds.
    bool attached = false;
};

// The process's one handle, so a second boot or attach is refused rather
// than stacked. Reads are the control thread's; nothing on the audio thread
// looks at it.
extern std::atomic<ClockworkEmbed*> g_clockwork_embed;
