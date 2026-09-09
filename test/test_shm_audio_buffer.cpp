// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_shm_audio_buffer.cpp — the audio capture region.
 *
 * The second-largest region in the arena (1.5 MB) and the second one with no
 * test: a rolling window of recent output that a client pulls at its own pace,
 * for recording, level meters, or anything that wants the audio without being
 * on the audio thread. The writer never blocks and never waits for a reader —
 * it laps one instead, which is the design and the thing most worth pinning.
 *
 * The contract that matters is what happens WHEN THE READER FALLS BEHIND. A
 * capture ring that silently returned lapped frames would hand a recorder a
 * file with a jump in it and no way to know; one that blocked would stall the
 * audio thread. This one resyncs to the oldest still-valid frame and REPORTS
 * how much it skipped, so a caller can decide — pad the gap, mark it, or give
 * up. That number arriving correctly is the difference between a recording
 * that is wrong and one that says where it is wrong.
 *
 * Written against the reader and writer directly rather than through a booted
 * engine, because both halves of the contract are here and neither needs a
 * device: a capture ring is a capture ring whatever filled it.
 */
#include "shm_audio_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

struct Slot {
    std::unique_ptr<uint8_t[]> bytes{ new uint8_t[sizeof(shm_audio_buffer)] };
    shm_audio_buffer* get() { return reinterpret_cast<shm_audio_buffer*>(bytes.get()); }
    Slot() { std::memset(bytes.get(), 0, sizeof(shm_audio_buffer)); }
};

float sampleAt(uint64_t frame, uint32_t ch) {
    return static_cast<float>(frame) + (ch == 0 ? 0.0f : 0.25f);
}

void writeBlock(shm_audio_buffer_writer& w, uint64_t from, uint32_t frames) {
    std::vector<float> l(frames), r(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        l[i] = sampleAt(from + i, 0);
        r[i] = sampleAt(from + i, 1);
    }
    const float* ptrs[2] = { l.data(), r.data() };
    w.write(ptrs, frames);
}

} // namespace

TEST_CASE("shm audio: frames pulled out are the frames written in",
          "[shm_audio]") {
    Slot slot;
    shm_audio_buffer_writer w(slot.get());
    REQUIRE(w.activate(2, 48000, 4096));

    shm_audio_buffer_reader r(slot.get());
    REQUIRE(r.is_active());
    REQUIRE(r.channels() == 2);
    REQUIRE(r.sample_rate() == 48000);
    r.seek_to_start();

    writeBlock(w, 0, 1024);

    std::vector<float> out(1024 * 2, -1.0f);
    uint64_t gap = 12345;                      // must be overwritten
    const uint32_t got = r.pull(out.data(), 1024, &gap);

    REQUIRE(got == 1024);
    REQUIRE(gap == 0);
    for (uint32_t i = 0; i < 1024; ++i) {
        REQUIRE(out[i * 2 + 0] == sampleAt(i, 0));
        REQUIRE(out[i * 2 + 1] == sampleAt(i, 1));
    }

    // A second pull with nothing new says so rather than repeating the block.
    REQUIRE(r.pull(out.data(), 1024, &gap) == 0);
    REQUIRE(gap == 0);
}

TEST_CASE("shm audio: a pull spanning the ring wrap is contiguous",
          "[shm_audio]") {
    Slot slot;
    shm_audio_buffer_writer w(slot.get());
    // A capacity the block size does NOT divide, so a write straddles the
    // boundary rather than always landing on it — the arrangement in which a
    // dropped modulo still places every sample correctly and the test proves
    // nothing. (It caught exactly that in the scope-stream ring.)
    const uint32_t cap = 3000;
    REQUIRE(w.activate(2, 48000, cap));
    REQUIRE(cap % 512 != 0);

    shm_audio_buffer_reader r(slot.get());
    r.seek_to_start();

    // Write past the wrap, pulling as we go so the reader never falls behind.
    std::vector<float> out(4096 * 2);
    uint64_t next = 0, pulled = 0, gap = 0;
    while (next < cap * 2) {
        writeBlock(w, next, 512);
        next += 512;
        uint32_t got;
        while ((got = r.pull(out.data(), 4096, &gap)) > 0) {
            REQUIRE(gap == 0);
            for (uint32_t i = 0; i < got; ++i) {
                REQUIRE(out[i * 2 + 0] == sampleAt(pulled + i, 0));
                REQUIRE(out[i * 2 + 1] == sampleAt(pulled + i, 1));
            }
            pulled += got;
        }
    }
    REQUIRE(pulled == next);
    REQUIRE(next > cap);          // the ring really did wrap
}

TEST_CASE("shm audio: a lapped reader is told exactly how much it lost",
          "[shm_audio]") {
    Slot slot;
    shm_audio_buffer_writer w(slot.get());
    const uint32_t cap = 2048;
    REQUIRE(w.activate(2, 48000, cap));

    shm_audio_buffer_reader r(slot.get());
    r.seek_to_start();

    // THE CASE THIS FILE EXISTS FOR. The writer does not wait, so a slow
    // reader gets lapped; what it must never do is hand back frames that look
    // continuous when they are not.
    const uint32_t written = cap * 3;
    for (uint32_t p = 0; p < written; p += 512) writeBlock(w, p, 512);

    std::vector<float> out(cap * 2);
    uint64_t gap = 0;
    const uint32_t got = r.pull(out.data(), cap, &gap);

    // Skipped everything but the last `cap` frames, and said so.
    REQUIRE(gap == written - cap);
    REQUIRE(got == cap);

    // And what came back is the NEWEST audio, not the oldest — a reader that
    // resynced to the wrong end would keep returning stale frames for ever
    // while reporting no gap at all.
    const uint64_t firstFrame = written - cap;
    for (uint32_t i = 0; i < got; ++i) {
        REQUIRE(out[i * 2 + 0] == sampleAt(firstFrame + i, 0));
        REQUIRE(out[i * 2 + 1] == sampleAt(firstFrame + i, 1));
    }
    // Caught up now, so the next pull is quiet rather than reporting the gap
    // a second time.
    REQUIRE(r.pull(out.data(), cap, &gap) == 0);
    REQUIRE(gap == 0);
}

TEST_CASE("shm audio: a partial pull leaves the rest for the next call",
          "[shm_audio]") {
    Slot slot;
    shm_audio_buffer_writer w(slot.get());
    REQUIRE(w.activate(2, 48000, 4096));
    shm_audio_buffer_reader r(slot.get());
    r.seek_to_start();

    writeBlock(w, 0, 1000);

    // A caller with a small buffer must not lose the remainder — the read
    // position advances by what was taken, not by what was available.
    std::vector<float> out(256 * 2);
    uint64_t gap = 0;
    uint64_t pulled = 0;
    for (int i = 0; i < 4; ++i) {
        const uint32_t got = r.pull(out.data(), 256, &gap);
        REQUIRE(gap == 0);
        REQUIRE(got == (i < 3 ? 256u : 232u));
        for (uint32_t f = 0; f < got; ++f)
            REQUIRE(out[f * 2 + 0] == sampleAt(pulled + f, 0));
        pulled += got;
    }
    REQUIRE(pulled == 1000);
}

TEST_CASE("shm audio: interleaved and per-channel writes agree",
          "[shm_audio]") {
    // Two entry points onto one ring, so a producer that already has
    // interleaved frames does not have to de-interleave them to hand them
    // over. They must place samples identically or a recording made through
    // one path and metered through the other would disagree.
    Slot a, b;
    shm_audio_buffer_writer wa(a.get()), wb(b.get());
    REQUIRE(wa.activate(2, 48000, 4096));
    REQUIRE(wb.activate(2, 48000, 4096));

    const uint32_t n = 777;
    std::vector<float> inter(n * 2);
    for (uint32_t i = 0; i < n; ++i) {
        inter[i * 2 + 0] = sampleAt(i, 0);
        inter[i * 2 + 1] = sampleAt(i, 1);
    }
    writeBlock(wa, 0, n);
    wb.write_interleaved(inter.data(), n);

    shm_audio_buffer_reader ra(a.get()), rb(b.get());
    ra.seek_to_start(); rb.seek_to_start();
    std::vector<float> oa(n * 2, 1.0f), ob(n * 2, 2.0f);
    REQUIRE(ra.pull(oa.data(), n) == n);
    REQUIRE(rb.pull(ob.data(), n) == n);
    REQUIRE(oa == ob);
}

TEST_CASE("shm audio: an oversized capacity is refused, not clamped silently",
          "[shm_audio]") {
    Slot slot;
    shm_audio_buffer_writer w(slot.get());
    // The data array is inline and fixed, so a capacity past it would have the
    // writer indexing outside the arena. Refusing at activate() is the only
    // moment a caller can be told; clamping would give it a ring quietly
    // shorter than the one it asked for and a recording quietly shorter than
    // the one it expected.
    REQUIRE_FALSE(w.activate(2, 48000, SHM_AUDIO_FRAMES + 1));
    shm_audio_buffer_reader r(slot.get());
    REQUIRE_FALSE(r.is_active());

    REQUIRE(w.activate(2, 48000, SHM_AUDIO_FRAMES));
    REQUIRE(r.is_active());
}
