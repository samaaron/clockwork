// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron

*/
/*
 * RingBufferWriter.h — Multi-producer ring buffer write (header-only)
 *
 * THE ONLY WRITER. A JavaScript copy of this arithmetic used to live beside
 * it, kept byte-identical by hand and pinned by nothing; a browser client now
 * calls this through clockwork_client_send instead, so there is one
 * implementation and no way for two of them to drift.
 *
 *  - 16-byte message header: magic(4) + length(4) + sequence(4) + sourceId(4)
 *  - frames NEVER wrap: a frame that doesn't fit before the end of the ring
 *    is preceded by a PADDING_MAGIC marker and restarts at offset 0, so every
 *    frame is contiguous and readers can parse in place
 *  - header length is EXACT (header + payload bytes); the frame footprint —
 *    and so the cursor advance — is that length rounded up to 4 bytes, with
 *    the pad bytes zeroed. Payload sizes round-trip exactly (MIDI messages
 *    are not 4-byte multiples) while offsets stay 4-aligned.
 *  - producers serialise via the write_lock spinlock (compare_exchange)
 *  - returns false when the frame doesn't fit (backpressure, no blocking)
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#endif
// src-relative so this header is usable under both the native (CMake) and web
// (emcc -Isrc) include roots.
#include "ring/ring.h"

class RingBufferWriter {
public:
    // Write one message into a ring buffer.
    // buffer_start: pointer to beginning of ring buffer region
    // buffer_size:  size of the ring buffer region (4-byte multiple)
    // head/tail:    atomic head/tail pointers from ControlPointers
    // sequence:     atomic sequence counter
    // write_lock:   atomic spinlock (0=unlocked)
    // data/size:    message payload
    //
    // Returns true if written, false if it doesn't fit. Note the fit test is
    // contiguous: a frame needs aligned_size bytes before the end of the ring
    // OR before the tail at offset 0 — total free space alone is not enough.
    // ── Reserving, then committing ──────────────────────────────────────────
    //
    // A caller that already holds its bytes calls write() below. A caller
    // whose bytes are not yet anywhere — a JavaScript client, whose encoder
    // would otherwise have to build the frame somewhere else and copy it in —
    // reserves room, writes into the ring directly, and commits the length it
    // actually used.
    //
    // THE RESERVATION HOLDS THE WRITE LOCK. Between reserve() and
    // commit()/abort() no other producer can write this ring, so the window
    // must contain nothing but filling the bytes. A caller that returns
    // without doing either wedges the ring for good.
    struct Reservation {
        uint8_t* payload = nullptr;   // where the caller writes
        uint32_t offset  = 0;         // frame start, after any wrap
        uint32_t max     = 0;         // payload bytes reserved
        bool valid() const { return payload != nullptr; }
    };

    // Room for up to `max_payload` bytes. Null payload on refusal, and the
    // lock is not held in that case.
    //
    // PLACEMENT IS DECIDED ON THE UPPER BOUND, because that is all that is
    // known yet: a reservation near the end of the ring wraps if `max_payload`
    // would not fit before the boundary, even if the committed message would
    // have. A tight bound therefore costs nothing; a loose one can waste the
    // tail of the ring at a wrap, and nothing else.
    static Reservation reserve(
        uint8_t*              buffer_start,
        uint32_t              buffer_size,
        std::atomic<int32_t>* head,
        std::atomic<int32_t>* tail,
        std::atomic<int32_t>* write_lock,
        uint32_t              max_payload)
    {
        Reservation r;
        const uint32_t total_size   = static_cast<uint32_t>(sizeof(Message)) + max_payload;
        const uint32_t aligned_size = (total_size + 3u) & ~3u;

        // Acquire spinlock
        int32_t expected = 0;
        while (!write_lock->compare_exchange_weak(expected, 1,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            expected = 0;
            #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
                _mm_pause();
            #elif defined(__aarch64__) || defined(__arm__)
                __asm__ volatile("yield");
            #endif
        }

        int32_t h = head->load(std::memory_order_relaxed);
        int32_t t = tail->load(std::memory_order_acquire);
        uint32_t uh = static_cast<uint32_t>(h);
        uint32_t ut = static_cast<uint32_t>(t);

        // Total free space (head==tail ambiguity costs one byte).
        uint32_t used  = (uh - ut + buffer_size) % buffer_size;
        uint32_t avail = buffer_size - used - 1;
        if (aligned_size > avail) {
            write_lock->store(0, std::memory_order_release);
            return r;
        }

        // Frames never wrap: if the frame doesn't fit before the end, mark the
        // remainder as padding and restart at offset 0 — which needs that much
        // contiguous room before the tail.
        uint32_t space_to_end = buffer_size - uh;
        if (aligned_size > space_to_end) {
            uint32_t space_at_front = (ut > 0) ? (ut - 1) : 0;
            if (aligned_size > space_at_front) {
                write_lock->store(0, std::memory_order_release);
                return r;
            }
            // Padding marker: magic word, zeros to the end of the ring. When
            // >= 16 bytes remain this doubles as a full zeroed pad header;
            // 4-byte alignment guarantees at least the magic always fits.
            // Written before the head moves, so an abandoned reservation
            // leaves it in space no reader has been told about.
            uint32_t pad = PADDING_MAGIC;
            std::memcpy(buffer_start + uh, &pad, sizeof(pad));
            if (space_to_end > sizeof(pad))
                std::memset(buffer_start + uh + sizeof(pad), 0,
                            space_to_end - sizeof(pad));
            uh = 0;
        }

        r.offset  = uh;
        r.max     = max_payload;
        r.payload = buffer_start + uh + sizeof(Message);
        return r;
    }

    // Publish `payload_bytes` of the reservation and release the lock.
    // payload_bytes must not exceed what was reserved; the frame advances the
    // head by what was USED, so an over-reservation costs nothing.
    static void commit(
        uint8_t*              buffer_start,
        uint32_t              buffer_size,
        std::atomic<int32_t>* head,
        std::atomic<int32_t>* sequence,
        std::atomic<int32_t>* write_lock,
        const Reservation&    r,
        uint32_t              payload_bytes,
        uint32_t              source_id)
    {
        const uint32_t total_size   = static_cast<uint32_t>(sizeof(Message)) + payload_bytes;
        const uint32_t aligned_size = (total_size + 3u) & ~3u;

        // The sequence is taken here rather than at reserve, so an abandoned
        // reservation does not leave a hole that reads as a lapped ring.
        uint32_t seq = static_cast<uint32_t>(sequence->fetch_add(1, std::memory_order_relaxed));

        // Header: length is the EXACT frame size; readers advance by its
        // 4-byte-aligned footprint.
        Message hdr;
        hdr.magic    = MESSAGE_MAGIC;
        hdr.length   = total_size;
        hdr.sequence = seq;
        hdr.sourceId = source_id;
        std::memcpy(buffer_start + r.offset, &hdr, sizeof(Message));

        // Zero the 0-3 alignment pad bytes (determinism: no stale ring bytes
        // inside a frame's footprint). The payload itself is already in place.
        if (aligned_size > total_size)
            std::memset(buffer_start + r.offset + total_size, 0,
                        aligned_size - total_size);

        head->store(static_cast<int32_t>((r.offset + aligned_size) % buffer_size),
                    std::memory_order_release);

        // WAKE WHOEVER IS WAITING ON THIS RING.
        //
        // A browser reader with nothing to do sleeps on the head with
        // Atomics.wait rather than spinning a core, so publishing without
        // waking leaves it asleep until something else happens to move the
        // head. The publisher is the only one who knows the head moved, so the
        // wake belongs here — a reader cannot wake itself.
        //
        // WebAssembly only, because it is the only place anything waits on
        // this address: native readers are woken through their own condition
        // variable (see RingReader), and a notify there would be a futex call
        // on a path that includes the audio thread. With no waiter the wasm
        // instruction is a few cycles and no call at all.
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_SHARED_MEMORY__)
        __builtin_wasm_memory_atomic_notify(
            reinterpret_cast<int32_t*>(head), 0x7FFFFFFFu);
#endif

        write_lock->store(0, std::memory_order_release);
    }

    // Give the reservation back without publishing. The head has not moved, so
    // nothing a reader can see was changed.
    static void abort(std::atomic<int32_t>* write_lock) {
        write_lock->store(0, std::memory_order_release);
    }

    // Write one message into a ring buffer, for a caller that already holds
    // the bytes. Reserve and commit in one step, which is the whole of it.
    static bool write(
        uint8_t*              buffer_start,
        uint32_t              buffer_size,
        std::atomic<int32_t>* head,
        std::atomic<int32_t>* tail,
        std::atomic<int32_t>* sequence,
        std::atomic<int32_t>* write_lock,
        const void*           data,
        uint32_t              data_size,
        uint32_t              source_id = 0)
    {
        Reservation r = reserve(buffer_start, buffer_size, head, tail,
                                write_lock, data_size);
        if (!r.valid())
            return false;

        std::memcpy(r.payload, data, data_size);
        commit(buffer_start, buffer_size, head, sequence, write_lock,
               r, data_size, source_id);
        return true;
    }
};
