// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * in_ring_door.h — the one way into the in ring (docs/RESTARTS.md, rule 1).
 *
 * Every door that writes the in ring in C writes through here:
 * clockwork_client_send and its two-step form, in any process, and
 * clockwork_ingress_write in the engine's. The page's channel restates it
 * (js/lib/osc_channel.js).
 *
 * The one question — is there an instance, or is the message clockwork's own,
 * which the engine answers either way? — is asked of the generation word,
 * and under the ring's writer lock. Asked
 * there, the mark a teardown sets (destroy_dsp) falls exactly between a
 * generation's messages and the ones after it: a writer that took the lock
 * before the mark wrote below it, and the next drain refuses its message as
 * that generation's; one that takes the lock after reads the word odd, and is
 * refused here.
 */
#pragma once

#include "clockwork_prefix.h"           // clockwork_sys_claims: whose message this is
#include "shared_memory.h"              // RingCursors, the generation word
#include "workers/RingBufferWriter.h"   // the writer, and its lock

#include <cstdint>
#include <cstring>

// Whether the in ring takes this message now.
inline bool in_ring_takes(const void* arena, const uint8_t* osc, uint32_t len) {
    return arenaHasInstance(arena) || clockwork_sys_claims(osc, len);
}

enum class InRingWrite { Written, Full, NoInstance };

// Write one message into the in ring of the arena at `arena`, whose ring is
// `ring_size` bytes at `ring`, as `origin`, if the ring takes it.
inline InRingWrite in_ring_write(const void* arena, uint8_t* ring, uint32_t ring_size,
                                 RingCursors* c, const uint8_t* osc, uint32_t len,
                                 uint32_t origin) {
    const auto r = RingBufferWriter::reserve(ring, ring_size, &c->in_head, &c->in_tail,
                                             &c->in_write_lock, len);
    if (!r.valid())
        return in_ring_takes(arena, osc, len) ? InRingWrite::Full : InRingWrite::NoInstance;
    if (!in_ring_takes(arena, osc, len)) {
        RingBufferWriter::abort(&c->in_write_lock);
        return InRingWrite::NoInstance;
    }
    std::memcpy(r.payload, osc, len);
    RingBufferWriter::commit(ring, ring_size, &c->in_head, &c->in_sequence, &c->in_write_lock,
                             r, len, origin);
    return InRingWrite::Written;
}
