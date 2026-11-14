// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2026 Sam Aaron

*/

/*
 * shm_peer_plane.h — the cross-process OSC command plane appended to the
 * native SHM segment (shm_segment.hpp): one trusted
 * external peer (e.g. a language runtime) sends commands and receives replies
 * through a pair of message-framed SPSC rings, using the same wire format as
 * every other clockwork ring (ring/ring.h; RingBufferWriter / ring_drain).
 *
 *   command ring: peer produces, the host's NRT gateway consumes and feeds
 *                 the ordinary ingest path (so routing and scheduling behave
 *                 exactly as they do for socket transports).
 *   reply ring:   the NRT gateway produces (ShmTransport — the gateway is the
 *                 sole transport caller, so single-producer holds by
 *                 construction), the peer consumes.
 *
 * Crash-safety is structural rather than recovered:
 *   - A ring's commit point is the producer's head release-store
 *     (RingBufferWriter): a producer that dies mid-write never publishes the
 *     partial frame. With exactly one producer per ring there is no lock to
 *     inherit from a corpse.
 *   - The host consumer (ring_drain) validates every header and treats the
 *     shared cursors as untrusted, so no peer state can make it read out of
 *     bounds or spin.
 *   - The host producer derives every write offset from its own head cursor;
 *     a hostile rep_tail can only cause reply drops, never out-of-bounds
 *     writes. Ring-full replies are dropped and counted (rep_dropped) — the
 *     host never blocks on peer state.
 *   - cmd_write_lock exists only so the peer can reuse the stock
 *     RingBufferWriter; with a single producer it is never contended. A peer
 *     that dies while holding it stalls nobody on the host (the host never
 *     takes it); shm_peer_attach() resets it, which is safe precisely
 *     because the plane is single-peer by contract — at attach time no live
 *     producer exists.
 *
 * Attach protocol (peer side): shm_peer_attach() stamps the pid, bumps the
 * generation, resets the writer lock, and skips any stale replies (they were
 * addressed to a dead process). Last attach wins. The host side never touches
 * the ownership fields and needs no liveness view of the peer: a dead peer
 * costs the host nothing.
 *
 * The host drain writes each command into the ingress ring labelled
 * SHM_PEER_WRITER; the label on the peer's own frame is ignored, never
 * trusted from shared memory.
 */
#pragma once

#include "ring/ring.h"   // ring_free_bytes

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

// Ring capacities. The command ring is sized for the largest payloads a client
// sends — a blob riding an OSC message, which is how definitions and samples
// arrive; frames never wrap, so the largest guaranteed-writable frame is
// bounded by the writer's contiguous-fit check, not by these totals.
constexpr uint32_t SHM_PEER_CMD_RING_SIZE = 256 * 1024;
constexpr uint32_t SHM_PEER_REP_RING_SIZE = 128 * 1024;

// The writer label on the peer's frames: the host drain puts it on every peer
// command it writes into the ingress ring, and ShmTransport on every frame of
// the reply ring. Distinctive value ("SHMP") so it reads clearly in logs and
// ring dumps.
constexpr uint32_t SHM_PEER_WRITER = 0x53484D50;

struct alignas(8) ShmPeerPlaneHeader {
    // ── ownership (peer-written; the host never touches these) ──────────────
    std::atomic<uint32_t> owner_pid;    // last attacher; informational
    std::atomic<uint32_t> generation;   // bumped by every attach

    // ── command ring cursors (peer = producer, host = consumer) ─────────────
    std::atomic<int32_t> cmd_head;
    std::atomic<int32_t> cmd_tail;
    std::atomic<int32_t> cmd_sequence;
    std::atomic<int32_t> cmd_write_lock;  // writer-convenience only; see file header

    // ── reply ring cursors (host = producer, peer = consumer) ───────────────
    // The host's writer serialisation lock is process-local (ShmTransport
    // member), deliberately NOT in shared memory: the peer cannot stall the
    // host by stomping a lock word.
    std::atomic<int32_t>  rep_head;
    std::atomic<int32_t>  rep_tail;
    std::atomic<int32_t>  rep_sequence;
    std::atomic<uint32_t> rep_dropped;    // replies dropped (ring full)

    // ── geometry (host-written at segment create; peer reads) ───────────────
    uint32_t cmd_ring_size;
    uint32_t rep_ring_size;

    // Set by the host while it holds commands back for room in the reply
    // ring; the peer's read that makes room clears it and wakes the host
    // (shm_peer_reply_room, shm_peer_room_made).
    std::atomic<uint32_t> rep_room_wanted;

    std::array<uint32_t, 3> _reserved;
};
static_assert(sizeof(ShmPeerPlaneHeader) == 64,
              "ShmPeerPlaneHeader must stay 64 bytes — the rings follow at fixed offsets");
// The Rust peer (rust/clockwork-comms/src/shm.rs, P_*) reads these by offset.
static_assert(offsetof(ShmPeerPlaneHeader, rep_tail) == 28 && offsetof(ShmPeerPlaneHeader, rep_dropped) == 36
              && offsetof(ShmPeerPlaneHeader, rep_ring_size) == 44 && offsetof(ShmPeerPlaneHeader, rep_room_wanted) == 48,
              "the Rust peer's plane offsets");

constexpr uint32_t SHM_PEER_PLANE_TOTAL_SIZE =
    static_cast<uint32_t>(sizeof(ShmPeerPlaneHeader))
    + SHM_PEER_CMD_RING_SIZE + SHM_PEER_REP_RING_SIZE;

// Ring bases. Sizes are compile-time constants on the host side — the host
// never trusts the header's geometry fields (they exist for the peer, which
// has no compile-time view of this header's build).
inline uint8_t* shm_peer_cmd_ring(ShmPeerPlaneHeader* h) {
    return reinterpret_cast<uint8_t*>(h + 1);
}
inline uint8_t* shm_peer_rep_ring(ShmPeerPlaneHeader* h) {
    return shm_peer_cmd_ring(h) + SHM_PEER_CMD_RING_SIZE;
}

// Host side, at segment create (the segment arrives zeroed; only the
// geometry needs explicit values).
inline void shm_peer_plane_init(ShmPeerPlaneHeader* h) {
    h->cmd_ring_size = SHM_PEER_CMD_RING_SIZE;
    h->rep_ring_size = SHM_PEER_REP_RING_SIZE;
}

// Peer side: claim the plane. Returns the new generation. See the attach
// protocol in the file header.
inline uint32_t shm_peer_attach(ShmPeerPlaneHeader* h, uint32_t pid) {
    h->owner_pid.store(pid, std::memory_order_relaxed);
    h->cmd_write_lock.store(0, std::memory_order_relaxed);
    // Skip replies addressed to a dead predecessor. Tail is consumer-owned;
    // moving it forward is always safe for the producer.
    h->rep_tail.store(h->rep_head.load(std::memory_order_acquire),
                      std::memory_order_release);
    return h->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// Host side: whether the reply ring has `need` free bytes. When it has not,
// the host holds the command back and the peer's next read wakes it: the flag
// goes up before the second look, and the peer looks at the flag after it
// moves rep_tail, so either the second look sees the room or the peer sees
// the flag.
inline bool shm_peer_reply_room(ShmPeerPlaneHeader* h, uint32_t need) {
    const auto room = [h] {
        return ring_free_bytes(static_cast<uint32_t>(h->rep_head.load(std::memory_order_relaxed)),
                               static_cast<uint32_t>(h->rep_tail.load(std::memory_order_acquire)),
                               SHM_PEER_REP_RING_SIZE);
    };
    if (room() >= need) return true;
    h->rep_room_wanted.store(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return room() >= need;
}

// Peer side, after a read has moved rep_tail: whether the host waits for the
// room the read made. The caller then wakes it (shm_command_client::wake_engine).
inline bool shm_peer_room_made(ShmPeerPlaneHeader* h) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return h->rep_room_wanted.load(std::memory_order_relaxed) != 0
        && h->rep_room_wanted.exchange(0, std::memory_order_relaxed) != 0;
}
