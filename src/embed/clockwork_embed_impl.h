// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The handle behind clockwork_embed.h, shared by the attach path
// (clockwork_embed.cpp) and the device boot (native/ClockworkEmbedDevice.cpp).
#pragma once
#include "clockwork_core.h"
#include "clockwork_embed.h"
#include <cstdint>
#include <memory>
#include <string>
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
    uint64_t rendered = 0;   // this host's count of the audio it has rendered: its block time's frames

    // The attach path's core, made and closed by this handle. A booted
    // handle's core is its engine's.
    ClockworkCore* core = nullptr;

    // The attach path's bulk lanes (docs/MEMORY.md), the assets area then the
    // returns area in one allocation, untouched until a client writes them;
    // and the guest's options. Both given to the core at every init, so a
    // reconfigure keeps them, and freed after the core.
    std::unique_ptr<uint8_t[]> lanes;
    uint32_t assetsBytes = 0, returnsBytes = 0;
    std::string guestConfig;
};
