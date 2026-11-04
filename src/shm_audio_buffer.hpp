// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//  Audio buffer — shared protocol header.
//
//  Fixed-layout single-producer / single-consumer ring of interleaved
//  float audio. The same struct is laid out in two storage segments:
//
//   - Native: inside the cross-process shm segment. A
//     GUI reads recordings while clockwork runs in a separate process.
//     C++ tests read the slots in-place.
//   - Web: inside the arena (the WASM SharedArrayBuffer). JS
//     and Playwright read directly via typed-array views.
//
//  One wire format, one reader/writer. A slot is a 32-byte header and its
//  frames right after it, as many as the core's layout gives (shared_memory.h,
//  clockwork::Layout): both segments are flat regions with no allocator.
//
//  The two slots are clockwork's taps — see "The taps" below. A guest's own
//  taps are its tap streams (shm_tap_stream.hpp); nothing of a guest's
//  is written here.

#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "memory_profile.h"   // CLOCKWORK_SCOPE_SAMPLE_RATE

namespace detail_shm_audio {

// ── The taps ────────────────────────────────────────────────────────────────
// Two slots, clockwork's, written inside every tick at the device edge:
//   OUT — what left for the device this block, after the guest and after
//         clockwork's own routing (port sinks, plugin-track sends).
//   IN  — what arrived from the device this block, as the guest saw it.
// The same on every host: the native callback, the worklet's quantum, a
// DAW's process. A client always has the master mix, and the input, with no
// guest cooperation, through the SCOPE entry of the arena table
// (clockwork_arena.h). Each slot carries `channels` live device channels, up
// to the ceiling its core's layout sized it for; `enabled` says the tap is
// live (the device has that direction); the cursor is monotonic and a
// reader catches up losslessly from its own position (shm_audio_buffer_reader).
// The taps flow from boot: nothing gates them.
inline constexpr uint32_t SCOPE_SLOTS       = 2;
inline constexpr uint32_t SCOPE_OUT_SLOT    = 0;
inline constexpr uint32_t SCOPE_IN_SLOT     = 1;

inline constexpr uint32_t SCOPE_SAMPLE_RATE = CLOCKWORK_SCOPE_SAMPLE_RATE;
// Per-slot header bytes (atomics + sizes + 64-bit write_position +
// padding), the frames following.
inline constexpr uint32_t SCOPE_HEADER_SIZE = 32;

// A slot's bytes for a ring of `frames` frames of `channels` channels: the
// header, the frames, 16-aligned so the next slot's header is.
inline constexpr uint64_t scope_slot_bytes(uint32_t frames, uint32_t channels) {
    return (uint64_t(SCOPE_HEADER_SIZE) + uint64_t(frames) * channels * sizeof(float) + 15u) & ~uint64_t(15u);
}

struct alignas(16) shm_audio_buffer {
    // Producer toggles this; consumers gate on it. 0 = idle / no writes,
    // 1 = active / writes flowing. Atomic because consumer may poll from
    // a different thread / process / browser worker.
    std::atomic<uint32_t> enabled;
    uint32_t              sample_rate;
    uint32_t              channels;
    uint32_t              capacity_frames;

    // Total frames the writer has produced since the slot was activated.
    // Reader subtracts capacity_frames to find the oldest still-readable
    // frame; falling more than capacity_frames behind = data loss (gap).
    std::atomic<uint64_t> write_position;

    // Pad to 32 bytes (header) so the frames start at a 16-aligned offset.
    std::array<uint32_t, 2> _reserved;

    // The interleaved float ring that follows the header
    // [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...], wrapping modulo capacity_frames.
    float*       frames()       { return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this) + SCOPE_HEADER_SIZE); }
    const float* frames() const { return reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(this) + SCOPE_HEADER_SIZE); }
};

static_assert(std::is_trivially_destructible<shm_audio_buffer>::value,
              "shm_audio_buffer must be trivially destructible (lives in shm)");
static_assert(sizeof(shm_audio_buffer) == SCOPE_HEADER_SIZE,
              "shm_audio_buffer is the slot's 32-byte header (frames 16-aligned)");

// ──── Producer side ─────────────────────────────────────────────────────
//
// Activate a slot at a given sample rate / channel count. Returns false
// if the slot pointer is null. After activate(), call write*() per audio
// block; deactivate() to stop writes.

class shm_audio_buffer_writer {
public:
    shm_audio_buffer_writer() = default;
    // `max_frames` and `max_channels`: the ring the slot was laid out for.
    shm_audio_buffer_writer(shm_audio_buffer* buf, uint32_t max_frames, uint32_t max_channels)
        : _buf(buf), _max_frames(max_frames), _max_channels(max_channels) {}

    bool valid() const { return _buf != nullptr; }

    // Format the slot and mark it live, within the ring it was laid out for.
    // The cursor starts again: a reader's position is relative to this.
    bool activate(uint32_t channels, uint32_t sample_rate,
                  uint32_t capacity_frames) {
        if (!_buf) return false;
        if (channels == 0 || channels > _max_channels) return false;
        if (capacity_frames == 0 || capacity_frames > _max_frames) return false;
        _buf->channels = channels;
        _buf->sample_rate = sample_rate;
        _buf->capacity_frames = capacity_frames;
        _buf->write_position.store(0, std::memory_order_relaxed);
        memset(_buf->frames(), 0, size_t(_max_frames) * _max_channels * sizeof(float));
        _buf->enabled.store(1, std::memory_order_release);
        return true;
    }

    // Appends num_frames of num_channels: the caller's count of the pointers
    // it passes, read once from the slot it filled them for. A slot wanting
    // more channels than that is not written — never read past what the
    // caller filled (the slot's count is read once: a second read could
    // exceed the first).
    void write(const float* const* channel_data, uint32_t num_channels, uint32_t num_frames) {
        if (!_buf || num_frames == 0) return;
        uint64_t pos = _buf->write_position.load(std::memory_order_relaxed);
        uint32_t cap = _buf->capacity_frames;
        uint32_t channels = _buf->channels;
        if (channels > num_channels) return;
        uint32_t slot = static_cast<uint32_t>(pos % cap);
        uint32_t first = (cap - slot < num_frames) ? (cap - slot) : num_frames;
        float* dst = _buf->frames() + static_cast<size_t>(slot) * channels;
        for (uint32_t f = 0; f < first; ++f) {
            for (uint32_t c = 0; c < channels; ++c)
                dst[f * channels + c] = channel_data[c][f];
        }
        if (first < num_frames) {
            uint32_t wrap = num_frames - first;
            dst = _buf->frames();
            for (uint32_t f = 0; f < wrap; ++f) {
                for (uint32_t c = 0; c < channels; ++c)
                    dst[f * channels + c] = channel_data[c][first + f];
            }
        }
        _buf->write_position.store(pos + num_frames, std::memory_order_release);
    }

    // Variant for already-interleaved input.
    void write_interleaved(const float* data, uint32_t num_frames) {
        if (!_buf || num_frames == 0) return;
        uint64_t pos = _buf->write_position.load(std::memory_order_relaxed);
        uint32_t cap = _buf->capacity_frames;
        uint32_t channels = _buf->channels;
        uint32_t slot = static_cast<uint32_t>(pos % cap);
        uint32_t first = (cap - slot < num_frames) ? (cap - slot) : num_frames;
        memcpy(_buf->frames() + static_cast<size_t>(slot) * channels, data,
               static_cast<size_t>(first) * channels * sizeof(float));
        if (first < num_frames) {
            memcpy(_buf->frames(), data + static_cast<size_t>(first) * channels,
                   static_cast<size_t>(num_frames - first) * channels * sizeof(float));
        }
        _buf->write_position.store(pos + num_frames, std::memory_order_release);
    }

    shm_audio_buffer* slot() const { return _buf; }

private:
    shm_audio_buffer* _buf = nullptr;
    uint32_t _max_frames = 0;
    uint32_t _max_channels = 0;
};

// ──── Consumer side ─────────────────────────────────────────────────────
//
// Reads new frames since the last call. Tracks read position locally;
// the writer's authoritative position is in the shm. If the reader falls
// more than capacity_frames behind, pull() reports a gap and resyncs.

class shm_audio_buffer_reader {
public:
    shm_audio_buffer_reader() = default;
    // `max_frames` and `max_channels` are the ring the slot's engine laid
    // out (the table's scope geometry). keepalive pins the mapping the slot
    // pointer reaches into (see shm_segment_client): reader copies survive
    // the client that minted them. Engine-side readers pass nothing.
    shm_audio_buffer_reader(shm_audio_buffer* buf, uint32_t max_frames, uint32_t max_channels,
                            std::shared_ptr<const void> keepalive = nullptr)
        : _buf(buf), _keepalive(std::move(keepalive)),
          _max_frames(max_frames), _max_channels(max_channels) {}

    bool valid() const { return _buf != nullptr; }
    bool is_active() const {
        return _buf && _buf->enabled.load(std::memory_order_acquire) != 0;
    }
    uint32_t sample_rate() const { return _buf ? _buf->sample_rate : 0; }
    uint32_t channels()    const { return _buf ? _buf->channels    : 0; }
    uint32_t capacity_frames() const { return _buf ? _buf->capacity_frames : 0; }

    // Snapshot the writer's current position. Useful for catching up
    // cleanly at the start of a session: call seek_to_live(), then loop
    // on pull() until done.
    uint64_t writer_position() const {
        return _buf ? _buf->write_position.load(std::memory_order_acquire) : 0;
    }

    void seek_to_live() { _last_read_pos = writer_position(); }

    // Reset to the absolute start of the buffer (only meaningful if the
    // reader was created before any writes happened). Used by tests that
    // want to capture from t=0 of a session.
    void seek_to_start() { _last_read_pos = 0; }

    // Pull up to max_frames of new audio. Output is interleaved
    // [ch0_f0, ch1_f0, ch0_f1, ...]. Returns frames written. If the
    // writer has lapped us by more than capacity, reports the gap and
    // resyncs to the oldest still-valid frame.
    uint32_t pull(float* out, uint32_t max_frames,
                  uint64_t* gap_frames_out = nullptr) {
        if (!_buf) return 0;
        uint64_t writer = _buf->write_position.load(std::memory_order_acquire);
        if (writer <= _last_read_pos) {
            if (gap_frames_out) *gap_frames_out = 0;
            return 0;
        }
        uint64_t avail = writer - _last_read_pos;
        // Untrusted, re-read from the segment: clamp to the engine's
        // allocation so a corrupt slot cannot index past its ring.
        uint32_t cap = _buf->capacity_frames;
        if (cap == 0 || cap > _max_frames) cap = _max_frames;
        uint32_t channels = _buf->channels;
        if (channels == 0 || channels > _max_channels) channels = _max_channels;
        if (cap == 0 || channels == 0) return 0;

        if (avail > cap) {
            uint64_t gap = avail - cap;
            if (gap_frames_out) *gap_frames_out = gap;
            _last_read_pos = writer - cap;
            avail = cap;
        } else if (gap_frames_out) {
            *gap_frames_out = 0;
        }

        uint32_t to_read = (avail < max_frames)
                              ? static_cast<uint32_t>(avail) : max_frames;
        if (to_read == 0) return 0;

        uint32_t slot = static_cast<uint32_t>(_last_read_pos % cap);
        uint32_t first = (cap - slot < to_read) ? (cap - slot) : to_read;
        memcpy(out, _buf->frames() + static_cast<size_t>(slot) * channels,
               static_cast<size_t>(first) * channels * sizeof(float));
        if (first < to_read) {
            memcpy(out + static_cast<size_t>(first) * channels, _buf->frames(),
                   static_cast<size_t>(to_read - first) * channels * sizeof(float));
        }
        _last_read_pos += to_read;
        return to_read;
    }

    uint64_t last_read_position() const { return _last_read_pos; }

private:
    shm_audio_buffer* _buf = nullptr;
    std::shared_ptr<const void> _keepalive;
    uint32_t      _max_frames = 0;
    uint32_t      _max_channels = 0;
    uint64_t      _last_read_pos = 0;
};

} // namespace detail_shm_audio

using detail_shm_audio::shm_audio_buffer;
using detail_shm_audio::shm_audio_buffer_writer;
using detail_shm_audio::shm_audio_buffer_reader;
using detail_shm_audio::SCOPE_SAMPLE_RATE;
using detail_shm_audio::SCOPE_HEADER_SIZE;
using detail_shm_audio::scope_slot_bytes;
using detail_shm_audio::SCOPE_SLOTS;
using detail_shm_audio::SCOPE_OUT_SLOT;
using detail_shm_audio::SCOPE_IN_SLOT;
