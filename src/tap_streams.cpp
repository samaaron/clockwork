// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * tap_streams.cpp — what the Rust tap crate needs from the host.
 *
 * Claiming and releasing a tap slot is Rust now
 * (rust/clockwork-taps/src/lib.rs). Two things it cannot know on its own
 * stay here: where shared memory currently is, and how this build carved up
 * the tap region. Both are answers, not logic — the geometry is a set of
 * build-time constants that change with the memory profile and that
 * JavaScript reads too, so restating them in Rust would be a third copy.
 */
#include <cstddef>
#include <cstdint>

#include "shared_memory.h"
#include "shm_tap_stream.hpp"
#include "audio_processor.h"   // get_shared_memory_base

extern "C" void* clockwork_shm_base() {
    return get_shared_memory_base();
}

extern "C" int clockwork_tap_geometry(size_t* start, size_t* header_size, size_t* slot_size,
                                 size_t* max_taps, uint32_t* ring_frames,
                                 uint32_t* channels, size_t* track_start, size_t* track_slots) {
    *start       = SHM_TAP_START;
    *header_size = SHM_TAP_HEADER_SIZE;
    *slot_size   = SHM_TAP_SLOT_SIZE;
    *max_taps  = SHM_TAP_MAX_TAPS;
    *ring_frames = SHM_TAP_RING_FRAMES;
    *channels    = SHM_TAP_STREAM_CHANNELS;
    *track_start = SHM_TRACK_TAPS_START;
    *track_slots = SHM_TRACK_TAPS_SLOTS;
    return 1;
}
