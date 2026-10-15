// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * tap_streams.h — what the tap streams' producer needs from the host.
 *
 * The slot claim and release live in Rust (rust/clockwork-taps); what
 * stays here is the pair of answers only the host can give: where shared
 * memory is, and how this build carved up the tap region.
 */
#ifndef CLOCKWORK_TAP_STREAMS_H
#define CLOCKWORK_TAP_STREAMS_H

#include <cstdint>
#include <cstddef>

extern "C" {
// Answers the Rust side asks the host for. Defined in tap_streams.cpp.
void* clockwork_shm_base(void);
// The tap region: a header, then the guest's slots, then the engine's
// track slots, every slot the same shape, one index space from 0.
int   clockwork_tap_geometry(size_t* start, size_t* headerSize, size_t* slotSize,
                        size_t* guestSlots, size_t* trackSlots,
                        unsigned* ringFrames, unsigned* channels);
}

#endif /* CLOCKWORK_TAP_STREAMS_H */
