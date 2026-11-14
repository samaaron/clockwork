// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork
 * Copyright (c) 2026 Sam Aaron
 *
 *
 * ShmTransport.h — the SHM command-plane transport: replies and notify pushes
 * for the segment's one trusted peer (shm_peer_plane.h), written into the
 * plane's reply ring. The counterpart of the gateway's command-ring drain.
 *
 * Single-producer by construction: the NRT gateway is the sole IOscTransport
 * caller, so the reply ring needs no shared lock — the writer-serialisation
 * word RingBufferWriter requires is a process-local member, deliberately not
 * in shared memory (the peer cannot stall the host by stomping it).
 *
 * The host never blocks on peer state: a full reply ring (slow or dead peer)
 * drops the packet and counts it in the plane's rep_dropped. The peer is the client, and what it asked to be told
 * is the core's to keep (lanes.h).
 *
 * Binds to the ENGINE'S plane slot (ClockworkEngine::peerPlaneSlot()) rather
 * than a plane pointer, loading it per call — so it follows the engine's
 * segment lifecycle (init publishes, shutdown nulls) with no re-wiring.
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "IOscTransport.h"
#include "shm_peer_plane.h"

class ShmTransport : public IOscTransport {
public:
    // Bind to the engine's published plane slot. A host calls this from its
    // startTransport hook — after engine.init(), which has already started the
    // egress drain thread that calls send()/writeReply(). The slot pointer is
    // therefore published across threads, so it is atomic (release here pairs
    // with the acquire loads in writeReply/ready).
    void bindPlaneSlot(std::atomic<ShmPeerPlaneHeader*>* slot) {
        mPlaneSlot.store(slot, std::memory_order_release);
    }

    // True once bound to a slot holding a live plane.
    bool ready() const {
        auto* slot = mPlaneSlot.load(std::memory_order_acquire);
        return slot && slot->load(std::memory_order_acquire) != nullptr;
    }

    // ── IOscTransport ──────────────────────────────────────────────────────────
    bool deliver(const uint8_t* data, uint32_t size) override { return writeReply(data, size); }
private:
    // Write one packet into the reply ring; drop when unbound, drop + count
    // (rep_dropped) when full.
    bool writeReply(const uint8_t* data, uint32_t size);
    // Points at the engine's plane slot; bound (main thread) after the egress
    // drain thread is already running, so the pointer itself is atomic.
    std::atomic<std::atomic<ShmPeerPlaneHeader*>*> mPlaneSlot{nullptr};

    // Host-side writer serialisation for the reply ring (see file header).
    std::atomic<int32_t> mRepWriteLock{0};
};
