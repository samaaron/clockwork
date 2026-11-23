// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * in_ring_door.h — the one way into the in ring (docs/RESTARTS.md, rule 1).
 *
 * Every door that writes the in ring in C writes through here:
 * clockwork_client_send and its two-step form, in any process, and
 * clockwork_ingress_write and clockwork_ingress_write_with in the engine's.
 * The page's channel restates it (js/lib/osc_channel.js).
 *
 * The one question — is there an instance, or is the message clockwork's own
 * and none of the guest's business, which the engine answers either way? —
 * is asked of the generation word, and under the ring's writer lock. Asked
 * there, the mark a teardown sets (destroy_dsp) falls exactly between a
 * generation's messages and the ones after it: a writer that took the lock
 * before the mark wrote below it, and the next drain refuses its message as
 * that generation's; one that takes the lock after reads the word odd, and is
 * refused here.
 */
#pragma once

#include "clockwork_prefix.h"           // clockwork_sys_answers_without_guest: whose message this is
#include "shared_memory.h"              // RingCursors, the generation word
#include "workers/RingBufferWriter.h"   // the writer, and its lock

#include <cstdint>
#include <cstring>

// Whether the in ring takes this message now.
inline bool in_ring_takes(const void* arena, const uint8_t* osc, uint32_t len) {
    return arenaHasInstance(arena) || clockwork_sys_answers_without_guest(osc, len);
}

// Unfilled: a message written in place whose writer wrote nothing, or said it
// wrote more than its room. Never the answer to a message written from bytes.
enum class InRingWrite { Written, Full, TooBig, NoInstance, Unfilled };

// Write one message into the in ring of the arena at `arena`, whose ring is
// `ring_size` bytes at `ring`, as `token`, where it is to lie: room for
// `max_bytes` is reserved, `fill(at, max_bytes)` writes the message there and
// answers how many bytes it wrote, and that many are committed if the ring
// takes the message.
//
// `fill` RUNS UNDER THE RING'S WRITER LOCK, which the audio thread's own
// writes wait on: it writes its bytes and does nothing else. So does
// `refused(at, len)`, called with the message the door would not take just
// before the room is given back, for a caller that must say what was refused.
//
// A ring with no room answers Full without asking who the message is for:
// nothing has been written to ask it of.
template <class Fill, class Refused>
InRingWrite in_ring_write_with(const void* arena, uint8_t* ring, uint32_t ring_size,
                               RingCursors* c, uint32_t max_bytes, uint32_t token,
                               Fill&& fill, Refused&& refused) {
    if (max_bytes > ring_max_message(ring_size)) return InRingWrite::TooBig;   // could stay unwritable for good (ring.h)
    const auto r = RingBufferWriter::reserve(ring, ring_size, &c->in_head, &c->in_tail,
                                             &c->in_write_lock, max_bytes);
    if (!r.valid()) return InRingWrite::Full;
    const uint32_t len = fill(r.payload, max_bytes);
    if (len == 0 || len > max_bytes) {
        RingBufferWriter::abort(&c->in_write_lock);
        return InRingWrite::Unfilled;
    }
    if (!in_ring_takes(arena, r.payload, len)) {
        refused(static_cast<const uint8_t*>(r.payload), len);
        RingBufferWriter::abort(&c->in_write_lock);
        return InRingWrite::NoInstance;
    }
    RingBufferWriter::commit(ring, ring_size, &c->in_head, &c->in_sequence, &c->in_write_lock,
                             r, len, token);
    return InRingWrite::Written;
}

// Write one message into the in ring of the arena at `arena`, whose ring is
// `ring_size` bytes at `ring`, as `token`, if the ring takes it. A full ring
// is Full only for a message the door would have taken.
inline InRingWrite in_ring_write(const void* arena, uint8_t* ring, uint32_t ring_size,
                                 RingCursors* c, const uint8_t* osc, uint32_t len,
                                 uint32_t token) {
    const InRingWrite wrote = in_ring_write_with(arena, ring, ring_size, c, len, token,
        [osc, len](uint8_t* at, uint32_t) { std::memcpy(at, osc, len); return len; },
        [](const uint8_t*, uint32_t) {});
    if (wrote == InRingWrite::Full && !in_ring_takes(arena, osc, len)) return InRingWrite::NoInstance;
    return wrote;
}
