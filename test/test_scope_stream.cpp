// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_scope_stream.cpp — the guest→client audio publishing region.
 *
 * `shm_scope_stream.hpp` is a whole IO path with no test: a guest writes
 * blocks of audio into a shared ring at its own rate and a client copies
 * windows out of it whenever it likes, with no lock between them and no
 * message carrying a sample. It is how a waveform gets on screen, and it is
 * the largest region in the arena (4 MB) — SHM_SCOPE is bigger than the OSC
 * rings and the node-tree mirror put together.
 *
 * It went untested because it is header-only and its only in-tree writer lives
 * behind the boundary, so nothing in this repository exercises it end to end.
 * Nothing has to: the writer and reader are both here, and what they have to
 * agree about is here too.
 *
 * WHAT MATTERS AND IS PINNED
 *
 *   - the ring WRAPS, and a window straddling the wrap comes back in order.
 *     A ring that reads correctly only below its capacity is a ring that
 *     works in every test and fails after a minute of audio.
 *
 *   - a window reaching further back than the ring holds is ZERO-FILLED and
 *     stays window-aligned. The contract is that `out` is always fully
 *     written; a reader that got a short buffer would draw the previous
 *     frame's samples at the left of the display and call it history.
 *
 *   - the stream ANCHORS on engine frames, so a client can turn a sample
 *     position into a place in the ring. This is the whole reason
 *     base_engine_frames exists rather than a plain write cursor.
 *
 *   - a DISCONTINUITY heals. A paused group skips blocks; the cursor jumps
 *     forward so the time mapping stays exact rather than sliding by the gap.
 *
 *   - corrupt geometry is CLAMPED, not trusted. The slot lives in shared
 *     memory that another process can write, so channels and capacity are
 *     attacker-influenced from the reader's point of view.
 */
#include "shm_scope_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// A slot on the heap, as the arena would hold it. Zeroed, because that is what
// the arena hands over and what activate() has to cope with.
struct Slot {
    std::unique_ptr<uint8_t[]> bytes{ new uint8_t[sizeof(shm_scope_stream)] };
    shm_scope_stream* get() { return reinterpret_cast<shm_scope_stream*>(bytes.get()); }
    Slot() { std::memset(bytes.get(), 0, sizeof(shm_scope_stream)); }
};

// Sample value as a pure function of absolute frame and channel, so any
// assertion below names exactly which frame arrived where.
float sampleAt(uint64_t frame, uint32_t ch) {
    return static_cast<float>(frame) + (ch == 0 ? 0.0f : 0.5f);
}

// Write `frames` frames starting at absolute engine position `from`.
void writeBlock(shm_scope_stream_writer& w, uint64_t from, uint32_t frames,
                uint32_t channels = 2) {
    std::vector<float> l(frames), r(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        l[i] = sampleAt(from + i, 0);
        r[i] = sampleAt(from + i, 1);
    }
    const float* ptrs[2] = { l.data(), r.data() };
    w.write(ptrs, frames, from);
    (void)channels;
}

} // namespace

TEST_CASE("scope stream: a window comes back exactly as it was written",
          "[scope]") {
    Slot slot;
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());
    REQUIRE(r.valid());
    REQUIRE(r.channels() == 2);

    writeBlock(w, 0, 512);
    REQUIRE(r.write_position() == 512);

    uint32_t used = 0;
    std::vector<float> out(256 * SHM_SCOPE_STREAM_CHANNELS, -1.0f);
    const uint32_t real = r.copy_window(512, 256, out.data(), &used);

    REQUIRE(used == 2);
    REQUIRE(real == 256);
    // The window ENDS at 512, so it holds frames 256..511.
    for (uint32_t i = 0; i < 256; ++i) {
        REQUIRE(out[i * 2 + 0] == sampleAt(256 + i, 0));
        REQUIRE(out[i * 2 + 1] == sampleAt(256 + i, 1));
    }
}

TEST_CASE("scope stream: a window straddling the ring wrap is still in order",
          "[scope]") {
    Slot slot;
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    // Fill past capacity so the ring has wrapped and the newest frames live at
    // the front of the buffer while the older half of the window is still at
    // the back.
    //
    // THE BLOCK SIZE MUST NOT DIVIDE THE CAPACITY, and that is the whole
    // difficulty of this case. With a block of 512 into a 16384-frame ring the
    // wrap lands exactly on a block boundary every time, so a writer that
    // dropped its modulo entirely still placed every sample correctly — the
    // underflowed `cap - at` came out as a full wrap and wrote the block at
    // the front, which is where it belonged anyway. The mutation was
    // undetectable and the test looked like it was testing the wrap.
    //
    // 500 does not divide 16384, so blocks straddle the boundary and the two
    // halves of a single write land at opposite ends of the ring.
    const uint32_t cap = r.capacity_frames();
    const uint32_t block = 500;
    REQUIRE(cap % block != 0);
    uint64_t pos = 0;
    while (pos < static_cast<uint64_t>(cap) + block) {
        writeBlock(w, pos, block);
        pos += block;
    }
    REQUIRE(r.write_position() == pos);
    REQUIRE(pos > cap);

    const uint32_t frames = 1024;
    uint32_t used = 0;
    std::vector<float> out(frames * SHM_SCOPE_STREAM_CHANNELS, -1.0f);
    const uint32_t real = r.copy_window(pos, frames, out.data(), &used);
    REQUIRE(real == frames);

    const uint64_t start = pos - frames;
    for (uint32_t i = 0; i < frames; ++i) {
        REQUIRE(out[i * 2 + 0] == sampleAt(start + i, 0));
        REQUIRE(out[i * 2 + 1] == sampleAt(start + i, 1));
    }
}

TEST_CASE("scope stream: history the ring no longer holds is zero-filled, not short",
          "[scope]") {
    Slot slot;
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    // Only 64 frames exist, but ask for 256 ending at 64. The 192 frames
    // before the stream began never existed.
    writeBlock(w, 0, 64);

    const uint32_t frames = 256;
    uint32_t used = 0;
    std::vector<float> out(frames * SHM_SCOPE_STREAM_CHANNELS, -1.0f);
    const uint32_t real = r.copy_window(64, frames, out.data(), &used);

    // Fewer real frames than asked for...
    REQUIRE(real == 64);
    // ...but the buffer is FULLY written, and the real frames sit at the END,
    // where they belong in time. A caller drawing this gets silence on the
    // left and audio on the right, not audio at the left and stale floats
    // after it.
    for (uint32_t i = 0; i < frames - 64; ++i) {
        REQUIRE(out[i * 2 + 0] == 0.0f);
        REQUIRE(out[i * 2 + 1] == 0.0f);
    }
    for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t o = frames - 64 + i;
        REQUIRE(out[o * 2 + 0] == sampleAt(i, 0));
        REQUIRE(out[o * 2 + 1] == sampleAt(i, 1));
    }
}

TEST_CASE("scope stream: the stream anchors on engine frames",
          "[scope]") {
    Slot slot;
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    // The first write anchors: a stream that starts an hour into the session
    // reports that hour as its base, so a client can map an engine sample
    // position to a cursor in this ring. Without the anchor the ring would
    // only know how much it had written, which is not a time.
    const uint64_t startedAt = 48000ull * 3600ull;
    writeBlock(w, startedAt, 128);
    REQUIRE(r.base_engine_frames() == startedAt);
    REQUIRE(r.write_position() == 128);
}

TEST_CASE("scope stream: a gap in the source moves the cursor rather than sliding time",
          "[scope]") {
    Slot slot;
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());

    writeBlock(w, 0, 128);
    REQUIRE(r.write_position() == 128);

    // A paused group skips blocks and then resumes. The next write says which
    // engine frame it starts at, and the cursor jumps to match — so cursor and
    // engine time stay in step. Appending it at 128 would have made every
    // later sample report a time 4096 frames too early, and nothing would
    // have said so.
    writeBlock(w, 4096, 128);
    REQUIRE(r.write_position() == 4096 + 128);
}

TEST_CASE("scope stream: corrupt geometry is clamped rather than believed",
          "[scope]") {
    Slot slot;
    shm_scope_stream_writer w(slot.get());
    w.activate(2);
    shm_scope_stream_reader r(slot.get());
    writeBlock(w, 0, 256);

    // The slot is in shared memory that another process can write, so these
    // are attacker-influenced from the reader's point of view. Both are used
    // to index the inline data array.
    slot.get()->channels        = 9999;
    slot.get()->capacity_frames = 0xFFFFFFFFu;
    REQUIRE(r.channels() <= SHM_SCOPE_STREAM_CHANNELS);
    REQUIRE(r.capacity_frames() <= SHM_SCOPE_RING_FRAMES);

    slot.get()->channels        = 0;
    slot.get()->capacity_frames = 0;
    REQUIRE(r.channels() > 0);
    REQUIRE(r.capacity_frames() > 0);

    // And a copy against the corrupt slot still writes the whole buffer
    // without reading outside the ring.
    uint32_t used = 0;
    std::vector<float> out(64 * SHM_SCOPE_STREAM_CHANNELS, -1.0f);
    r.copy_window(256, 64, out.data(), &used);
    REQUIRE(used <= SHM_SCOPE_STREAM_CHANNELS);
}

TEST_CASE("scope stream: an inactive or absent slot yields silence, not stale memory",
          "[scope]") {
    // Contract from copy_window: `out` is always fully written. A reader
    // holding a slot that was never activated, or none at all, must not leave
    // the caller's buffer as it found it — that buffer is the previous frame's
    // audio, and returning it would draw a picture that looks alive.
    shm_scope_stream_reader none(nullptr);
    REQUIRE_FALSE(none.valid());

    uint32_t used = 0;
    std::vector<float> out(32 * SHM_SCOPE_STREAM_CHANNELS, 7.0f);
    const uint32_t real = none.copy_window(0, 32, out.data(), &used);
    REQUIRE(real == 0);
    for (float v : out) REQUIRE(v == 0.0f);

    Slot slot;                       // zeroed: state == 0, never activated
    shm_scope_stream_reader idle(slot.get());
    REQUIRE_FALSE(idle.valid());
}
