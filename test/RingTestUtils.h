// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * RingTestUtils.h — helpers for asserting on what a scheduler fires.
 *
 * Tests drive the scheduler (or MidiClockOut feeding it), drain the events due
 * at a given time, and assert on the captured OSC packets. Draining pops each
 * due event and releases its slot — the same popDue/release the engine's fire
 * loop performs — so the events captured here are exactly the ones the fire
 * loop would dispatch.
 *
 * `Scheduler` is duck-typed, exactly as clockwork_fire_due() duck-types it: anything
 * with popDue/release fits. Naming a concrete scheduler here once broke every
 * test that included this header, whether it used a scheduler or not, when
 * that class moved; clockwork's own instantiation is EngineScheduler
 * (src/scheduler/engine_schedule.h) and this header still does not name it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ring_test {

// One event the scheduler returned as due, captured for assertions. `data` is a
// copy of the scheduled OSC packet, so it stays valid after the slot is freed.
struct Fired {
    int64_t              when   = 0;
    uint32_t             origin = 0;
    std::vector<uint8_t> data;
};

// Pop every event due at/through `now`, in fire order, releasing each slot.
template <class Scheduler>
inline std::vector<Fired> drainDue(Scheduler& es, int64_t now) {
    std::vector<Fired> out;
    for (;;) {
        auto e = es.popDue(now);
        if (!e.valid()) break;
        out.push_back(Fired{e.when, e.meta->origin,
                            std::vector<uint8_t>(e.data, e.data + e.size)});
        es.release(e);
    }
    return out;
}

// True if `data` begins with the NUL-terminated OSC address `addr`.
inline bool addrEquals(const std::vector<uint8_t>& data, const char* addr) {
    for (std::size_t i = 0; i < data.size(); ++i) {
        const char c = static_cast<char>(data[i]);
        if (c != addr[i]) return false;
        if (c == '\0')    return true;
    }
    return false;   // no NUL terminator within the packet
}

// Count captured events whose OSC address equals `addr`.
inline int countByAddr(const std::vector<Fired>& fired, const char* addr) {
    int n = 0;
    for (const auto& f : fired)
        if (addrEquals(f.data, addr)) ++n;
    return n;
}

} // namespace ring_test
