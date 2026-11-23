// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * EventIngress.h — an event a subsystem produced, into the in ring.
 *
 * What a port sent, or a pad: produced on the subsystem's own thread (for
 * MIDI the system's, which must not allocate), and written into the in ring
 * where it is to lie (lanes.h, clockwork_ingress_write_with), to be answered
 * on the audio thread by clockwork_event_route. The subsystem hands over the
 * room the event takes and the function that writes it; nothing is built
 * first and copied in.
 */
#pragma once

#include "clockwork_config.h"   // clockwork_log
#include "lanes/lanes.h"        // clockwork_ingress_write_with
#include "log_limit.h"

#include <cstdint>
#include <cstring>

// Write the event `fill` makes, of at most `room` bytes, into `core`'s in
// ring. One the ring does not take is dropped, and said as `dropped` allows
// with `what` it was ("MIDI", "gamepad") and its address: written again for
// the line, here and off the ring, since the ring kept none of it.
inline void clockwork_event_ingress(ClockworkCore* core, uint32_t room, ClockworkFill fill, void* fill_ctx,
                                    clockwork::LogLimit& dropped, const char* what) {
    if (clockwork_ingress_write_with(core, room, 0, fill, fill_ctx)) return;
    if (!dropped.say()) return;
    uint8_t again[256];
    const uint32_t n = fill ? fill(fill_ctx, again, sizeof again) : 0;
    const uint32_t wrote = n <= sizeof again ? n : 0;
    const auto address = static_cast<int>(strnlen(reinterpret_cast<const char*>(again), wrote));
    clockwork_log(core, "WARNING: %s event %.*s dropped — IN ring full", what, address,
                  reinterpret_cast<const char*>(again));
}
