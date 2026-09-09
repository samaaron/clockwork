// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * peer_shm/main.cpp — out-of-process SHM command-plane peer used by
 * test_shm_peer_crash.cpp. Attaches to a running engine's segment and writes
 * /clockwork/clock/tempo/get <id> frames into the peer command ring exactly as
 * a real peer would: shm_segment_client + shm_peer_attach + RingBufferWriter.
 * The engine echoes each id as the last argument of its tempo.reply, which is
 * how the parent checks completeness and order.
 *
 * Modes (prints "ready" on stdout once attached):
 *   burst <endpoint> <count>   write ids 1..count (retry on ring-full), exit 0
 *   spam <endpoint>            write frames with increasing ids until killed
 *   hold-lock <endpoint>       set cmd_write_lock and spin — a corpse holding the
 *                          writer lock, for the attach-recovery test
 */
#include "shm_segment.hpp"
#include "shm_attach.hpp"
#include "shm_peer_plane.h"
#include "workers/RingBufferWriter.h"
#include "clockwork_prefix.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {

// A minimal OSC "<address> <int32 id>" message: padded address, ",i", the
// id big-endian. Returns the encoded length.
uint32_t encodeQuery(uint8_t* out, size_t cap, int32_t id) {
    static const char* const kAddr = CLOCKWORK_SYS("clock/tempo/get");
    const size_t alen   = std::strlen(kAddr) + 1;
    const size_t padded = (alen + 3) & ~size_t(3);
    const size_t total  = padded + 4 + 4;
    if (total > cap) return 0;
    std::memset(out, 0, total);
    std::memcpy(out, kAddr, alen);
    std::memcpy(out + padded, ",i\0\0", 4);
    uint8_t* v = out + padded + 4;
    v[0] = static_cast<uint8_t>(id >> 24);
    v[1] = static_cast<uint8_t>(id >> 16);
    v[2] = static_cast<uint8_t>(id >> 8);
    v[3] = static_cast<uint8_t>(id);
    return static_cast<uint32_t>(total);
}

// Write one frame, retrying on ring-full (the engine drains every block).
void writeQuery(ShmPeerPlaneHeader* plane, int32_t id) {
    uint8_t msg[64];
    const uint32_t len = encodeQuery(msg, sizeof msg, id);
    while (!RingBufferWriter::write(
               shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
               &plane->cmd_head, &plane->cmd_tail,
               &plane->cmd_sequence, &plane->cmd_write_lock,
               msg, len, 0)) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <burst|spam|hold-lock> <endpoint> [count]\n", argv[0]);
        return 2;
    }
    const std::string mode     = argv[1];
    const std::string endpoint = argv[2];

    ShmPeerPlaneHeader* plane = nullptr;
    try {
        // Across a real process boundary, the way a real peer does it: the
        // engine hands its anonymous segment over the attach endpoint. The
        // client mapping must outlive every plane access; keep it for the
        // whole process lifetime.
        std::string err;
        const auto handle = shm_attach::receive(endpoint, &err);
        if (!detail_shm_segment::shm_handle_valid(handle)) {
            std::fprintf(stderr, "peer_shm: attach at %s failed: %s\n", endpoint.c_str(), err.c_str());
            return 3;
        }
        static shm_segment_client client(handle);
        plane = client.get_peer_plane();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "peer_shm: cannot map the segment from %s: %s\n", endpoint.c_str(), e.what());
        return 3;
    }
    if (!plane) return 3;

    if (mode == "probe") {
        std::printf("gen=%u pid=%u cmd_head=%d cmd_tail=%d cmd_lock=%d "
                    "rep_head=%d rep_tail=%d rep_dropped=%u\n",
                    plane->generation.load(), plane->owner_pid.load(),
                    plane->cmd_head.load(), plane->cmd_tail.load(),
                    plane->cmd_write_lock.load(),
                    plane->rep_head.load(), plane->rep_tail.load(),
                    plane->rep_dropped.load());
        return 0;
    }

    shm_peer_attach(plane, static_cast<uint32_t>(getpid()));
    // hold-lock: take the lock BEFORE announcing readiness — the parent kills
    // us the moment it reads "ready", and the corpse must be holding the lock.
    if (mode == "hold-lock")
        plane->cmd_write_lock.store(1, std::memory_order_relaxed);
    std::printf("ready\n");
    std::fflush(stdout);

    if (mode == "burst") {
        const int count = (argc > 3) ? std::atoi(argv[3]) : 100;
        for (int32_t id = 1; id <= count; ++id)
            writeQuery(plane, id);
        return 0;
    }
    if (mode == "spam") {
        for (int32_t id = 1;; ++id)
            writeQuery(plane, id);
    }
    if (mode == "hold-lock") {
        for (;;)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fprintf(stderr, "peer_shm: unknown mode %s\n", mode.c_str());
    return 2;
}
