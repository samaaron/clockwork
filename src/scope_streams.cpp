// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * scope_streams.cpp — what the Rust scope crate needs from the host.
 *
 * Claiming and releasing a scope slot is Rust now
 * (rust/clockwork-scope/src/lib.rs). Two things it cannot know on its own
 * stay here: where shared memory currently is, and how this build carved up
 * the scope region. Both are answers, not logic — the geometry is a set of
 * build-time constants that change with the memory profile and that
 * JavaScript reads too, so restating them in Rust would be a third copy.
 */
#include <cstddef>
#include <cstdint>

#include "shared_memory.h"
#include "shm_scope_stream.hpp"
#include "audio_processor.h"   // get_shared_memory_base

extern "C" void* clockwork_shm_base() {
    return get_shared_memory_base();
}

extern "C" int clockwork_scope_geometry(size_t* start, size_t* header_size, size_t* slot_size,
                                 size_t* max_scopes, uint32_t* ring_frames,
                                 uint32_t* channels, size_t* track_start, size_t* track_slots) {
    *start       = SHM_SCOPE_START;
    *header_size = SHM_SCOPE_HEADER_SIZE;
    *slot_size   = SHM_SCOPE_SLOT_SIZE;
    *max_scopes  = SHM_SCOPE_MAX_SCOPES;
    *ring_frames = SHM_SCOPE_RING_FRAMES;
    *channels    = SHM_SCOPE_STREAM_CHANNELS;
    *track_start = SHM_TRACK_TAPS_START;
    *track_slots = SHM_TRACK_TAPS_SLOTS;
    return 1;
}
