// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/*
 * lanes_internal.h — engine-internal lanes plumbing (not host API).
 *
 * clockwork_egress_nrt_write: the NRT egress *producer*. Engine control-thread
 * subsystems (the OscEgress framer; embedded MIDI/Link tasks) frame
 * [route][osc] into the NRT-out ring through here. It is NOT part of the host
 * ABI — hosts only ever drain egress (clockwork_egress_nrt_drain). It lives here, not
 * in lanes.h, so the public boundary stays strictly write-ingress /
 * read-egress / tick.
 *
 * clockwork_lanes_reset_in_ring / _rt_out_ring / _nrt_out_ring: a fresh epoch
 * for one ring — its head, tail and sequence zeroed — serialising against its
 * producers via their writer spinlocks, since IN and NRT-out producers keep
 * running through a rebuild and a bare store would race an in-flight write.
 * The in ring's at every build; the reply rings' at a new arena only
 * (docs/RESTARTS.md). An egress ring's drain state (sequence-gap tracking)
 * restarts with it — otherwise a second engine boot in the same process
 * counts a spurious sequence gap.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../clockwork_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One frame onto the NRT egress ring. False when the ring is full, or when len is more than a frame can carry
   (clockwork_egress_nrt_max_frame): the ring is a fixed size, and a frame
   has to be able to drain. Either refusal is counted
   (PerformanceMetrics::egress_frames_refused). */
bool clockwork_egress_nrt_write(ClockworkCore* core, const uint8_t* osc, uint32_t len);

/* The most OSC bytes one NRT egress frame of `core` carries: its ring's. */
uint32_t clockwork_egress_nrt_max_frame(const ClockworkCore* core);

/* Whether the NRT egress ring has `bytes` free (0 before init): what the
   control pass checks before it answers a verb, holding the verb back rather
   than have its answers refused. When it has not, the ring's next reader to
   make room wakes the pass (clockwork_egress_nrt_room_made). */
int clockwork_egress_nrt_room(ClockworkCore* core, uint32_t bytes);

/* Bytes in the NRT egress ring that no reader has taken yet (0 before init):
   frames on their way to a client, which the next egress drain hands to the
   transport. */
uint32_t clockwork_egress_nrt_pending(ClockworkCore* core);

/* Ask to be woken by the next reader to make room, for a producer that holds
   a frame after a refused write: it writes once more after asking, for room
   made in between. */
void clockwork_egress_nrt_want_room(ClockworkCore* core);

/* Every reader of the NRT egress ring, after it gives room back
   (clockwork_ring_give_back): wakes the control pass if it holds work for
   room in the ring (clockwork_control_wake). */
void clockwork_egress_nrt_room_made(ClockworkCore* core);

/* Block until the engine has said something since `*seen` — a block that
   wrote the RT egress ring, an NRT egress write, or clockwork_egress_wake —
   or `timeout_ms` passes (UINT32_MAX: no deadline), or the core begins to
   close. `*seen` becomes the count now. 1 when it moved, 0 when the time
   passed first, -1 when the core closed under the wait (and at once for a
   core closing or gone). The wait holds the core (clockwork_core.h). Any
   thread but the audio thread; what clockwork_client_wait is in the engine's
   process. */
int clockwork_egress_wait(ClockworkCore* core, uint32_t* seen, uint32_t timeout_ms);

/* Move the count and wake every waiter, as if the engine had said something:
   for a host ending a wait it parked (clockwork_client_wake). Any thread. */
void clockwork_egress_wake(ClockworkCore* core);

/* Count `n` deliveries the transport could not make for a frame that left an
   egress ring (PerformanceMetrics::egress_frames_undelivered): whoever hands
   egress to a transport, after each frame. */
void clockwork_egress_count_undelivered(ClockworkCore* core, uint32_t n);

void clockwork_lanes_reset_in_ring(ClockworkCore* core);
void clockwork_lanes_reset_rt_out_ring(ClockworkCore* core);
void clockwork_lanes_reset_nrt_out_ring(ClockworkCore* core);

#ifdef __cplusplus
}

#include "../in_ring_door.h"

struct RingPart;   // workers/RingBufferWriter.h

// clockwork_egress_nrt_write with the OSC laid down from `parts` end to end,
// nothing gathered first: for a frame made of pieces held apart. The same
// refusals, counted the same.
bool clockwork_egress_nrt_write_parts(ClockworkCore* core,
                                      const RingPart* parts, uint32_t n_parts);

// clockwork_ingress_write, saying which of its two refusals it made: full (a
// moment: try again) or no instance (answered: the refusal is on the NRT
// egress, and the message is done with). For a door of the
// engine's own that holds a message while the ring is full and must not hold
// one that has been answered — a shm peer's plane.
InRingWrite clockwork_ingress_write_status(ClockworkCore* core, const uint8_t* osc, uint32_t len, uint32_t writer);
#endif
