// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron


    Message-framed ring buffer primitives: the 16-byte frame header and the magic
    constants shared by the writer (RingBufferWriter.h) and the reader
    (ring_drain.h). Frames never wrap the ring boundary — a frame that would cross
    the end is preceded by a PADDING_MAGIC marker and restarts at offset 0.
    This is the only definition of the frame. Clients in other languages reach
    it through src/clockwork_client.h rather than restating it — except a
    browser thread outside the audio worklet, which cannot run the C and uses
    js/lib/ring.js (docs/THREADS.md), held to this by the ring tests.
*/

#pragma once

#include <cstdint>

// 16-byte frame header; payload follows.
struct alignas(4) Message {
    uint32_t magic;       // MESSAGE_MAGIC for validation
    uint32_t length;      // total frame size including this header
    uint32_t sequence;    // sequence number for ordering
    uint32_t sourceId;    // writer/origin token (web: 0 = main thread, 1+ =
                          // workers; native: the transport origin token)
    // payload follows (binary data — OSC on every ring here)
};

// Free bytes in a ring of `size` bytes whose writer is at `head` and whose
// reader has given back up to `tail` (head == tail is empty, so a full ring
// keeps one byte). The one formula every writer and every room check uses.
inline uint32_t ring_free_bytes(uint32_t head, uint32_t tail, uint32_t size) {
    const uint32_t used = (head - tail + size) % size;
    return size - used - 1;
}

constexpr uint32_t MESSAGE_MAGIC = 0xDEADBEEF;
constexpr uint32_t PADDING_MAGIC = 0xBADDCAFE;  // end-of-ring pad marker; frame restarts at offset 0
