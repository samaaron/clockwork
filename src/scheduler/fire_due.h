// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork
 * Copyright (c) 2025 Sam Aaron
 *
 *
 * fire_due.h — the shared scheduler fire loop. Pop every event due at/through
 * `nextTime` in time order and hand each to `dispatch` with its threaded metadata
 * (origin token + timetag) plus this block's start. The engine and the standalone
 * host run the SAME loop; only the dispatch sink differs (the engine routes
 * through clockwork/DSP boundary, which hands anything the reserved prefix does
 * not claim to the DSP; the host through its own clockwork routes, with no DSP
 * attached). `Scheduler` is duck-typed: popDue → Event{valid, data, size,
 * when, meta->origin} → release.
 */
#pragma once

#include <cstdint>

template <class Scheduler, class DispatchFn>
inline void clockwork_fire_due(Scheduler& sched, int64_t nextTime, int64_t blockTime,
                        DispatchFn&& dispatch) {
    for (;;) {
        auto ev = sched.popDue(nextTime);
        if (!ev.valid()) break;
        dispatch(ev.data, ev.size, ev.meta->origin, ev.when, blockTime);
        sched.release(ev);
    }
}
