// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * ShmTransport.cpp — see ShmTransport.h. Reply-ring frames are standard
 * Message-framed OSC (ring/ring.h wire format, sourceId = origin token, no
 * route word): the peer drains them with the stock reader protocol.
 */
#include "ShmTransport.h"

#include "RingBufferWriter.h"

bool ShmTransport::writeReply(const uint8_t* data, uint32_t size) {
    auto* slot = mPlaneSlot.load(std::memory_order_acquire);
    if (!slot) return false;
    ShmPeerPlaneHeader* plane = slot->load(std::memory_order_acquire);
    if (!plane) return false;
    bool ok = RingBufferWriter::write(
        shm_peer_rep_ring(plane), SHM_PEER_REP_RING_SIZE,
        &plane->rep_head, &plane->rep_tail, &plane->rep_sequence,
        &mRepWriteLock,
        data, size, SHM_PEER_ORIGIN_TOKEN);
    if (!ok)
        plane->rep_dropped.fetch_add(1, std::memory_order_relaxed);
    return ok;
}
