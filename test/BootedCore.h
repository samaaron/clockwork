// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * BootedCore.h — a core a case makes, boots with clockwork_init and closes,
 * with memory of its own for its guest, and a block rendered on the calling
 * thread. For the binaries whose cases make every core the process holds.
 */
#pragma once

#include "lanes/lanes.h"
#include "clockwork_arena.h"
#include "core_state.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

namespace core_test {

constexpr double   kRate  = 48000.0;
constexpr uint32_t kBlock = 128;

// Laid out from `profile`, or from the target's own sizes with none.
// `guest`: whether its build must have its guest.
struct Core {
    struct alignas(16) Memory { uint8_t bytes[1 << 20]; };
    std::unique_ptr<Memory> memory = std::make_unique<Memory>();
    ClockworkCore* core = nullptr;
    explicit Core(const ClockworkProfile* profile = nullptr, bool guest = true) {
        ClockworkStatus st = CLOCKWORK_OK;
        core = clockwork_core_new(&st);
        REQUIRE(core != nullptr);
        if (profile) {
            const char* why = clockwork_set_profile(core, profile);
            INFO((why ? why : ""));
            REQUIRE(why == nullptr);
        }
        clockwork_init(core, kRate, kBlock, 2, 2, 0, memory->bytes, sizeof memory->bytes, nullptr, 0, 0);
        if (guest) REQUIRE(clockwork_sample_rate(core) == kRate);
    }
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    ~Core() { clockwork_core_close(core); }

    const ClockworkArenaHeader* header() const { return clockwork_arena_header(core); }
    const ClockworkArenaEntry& region(uint32_t id) const {
        const ClockworkArenaEntry* e = clockwork_arena_find(header(), id);
        REQUIRE(e != nullptr);
        return *e;
    }
};

inline ClockworkTick tickHere(ClockworkCore* core) {
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    return clockwork_tick(core, &time, 2, 2);
}

} // namespace core_test
