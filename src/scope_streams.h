// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * scope_streams.h — what the scope streams' producer needs from the host.
 *
 * The slot claim and release live in Rust (rust/clockwork-scope); what
 * stays here is the pair of answers only the host can give: where shared
 * memory is, and how this build carved up the scope region.
 */
#ifndef CLOCKWORK_SCOPE_STREAMS_H
#define CLOCKWORK_SCOPE_STREAMS_H

#include <cstdint>
#include <cstddef>

extern "C" {
// Answers the Rust side asks the host for. Defined in scope_streams.cpp.
void* clockwork_shm_base(void);
// The guest's scope region, and the engine's track taps: the same slot
// shape, numbered after the guest's (trackFirst == maxScopes).
int   clockwork_scope_geometry(size_t* start, size_t* headerSize, size_t* slotSize,
                        size_t* maxScopes, unsigned* ringFrames, unsigned* channels,
                        size_t* trackStart, size_t* trackSlots);
}

#endif /* CLOCKWORK_SCOPE_STREAMS_H */
