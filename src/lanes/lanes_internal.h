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

#ifdef __cplusplus
extern "C" {
#endif

/* token is the origin sourceId the frame replies to (broadcasts ignore it).
   False when the ring is full, or when len is more than a frame can carry
   (clockwork_egress_nrt_max_frame): the ring is a fixed size, and a frame
   has to be able to drain. */
bool clockwork_egress_nrt_write(uint32_t route, uint32_t token,
                         const uint8_t* osc, uint32_t len);

/* The most OSC bytes one NRT egress frame carries. */
uint32_t clockwork_egress_nrt_max_frame(void);

/* Free bytes in the NRT egress ring now (0 before init): what a producer
   that would rather wait than have a frame refused checks first — the
   control pass, before it answers a verb. */
uint32_t clockwork_egress_nrt_free(void);

void clockwork_lanes_reset_in_ring(void);
void clockwork_lanes_reset_rt_out_ring(void);
void clockwork_lanes_reset_nrt_out_ring(void);

#ifdef __cplusplus
}

#include "../in_ring_door.h"

// clockwork_ingress_write, saying which of its two refusals it made: full (a
// moment: try again) or no instance (answered: the refusal is on the NRT
// egress for the source, and the message is done with). For a door of the
// engine's own that holds a message while the ring is full and must not hold
// one that has been answered — a shm peer's plane.
InRingWrite clockwork_ingress_write_status(const uint8_t* osc, uint32_t len, uint32_t source_id);
#endif
