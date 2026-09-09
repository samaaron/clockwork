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
//   - Web: inside ring_buffer_storage (the WASM SharedArrayBuffer). JS
//     and Playwright read directly via typed-array views.
//
//  One wire format, one reader/writer. The data array is inline because
//  both segments are flat fixed-layout regions with no dynamic allocator.
//
//  The two slots are clockwork's taps — see "The taps" below. A guest's own
//  taps are its scope streams (shm_scope_stream.hpp); nothing of a guest's
//  is written here.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

// memory_profile.h is a pure-macro leaf header with no further dependencies,
// so including it here keeps this header standalone while ensuring every
// translation unit — whether it reaches this header via shared_memory.h or
// directly (native shm segment, C++ tests) — sees the same capture-ring sizing.
#include "memory_profile.h"

namespace detail_shm_audio {

// Slot count and per-slot ring sizing come from memory_profile.h. Slots must
// be small enough that the inline data arrays fit inside ring_buffer_storage /
// The shm segment at the configured ring size. CLOCKWORK_SHM_AUDIO_SECONDS sets the
// duration (1s in production; test builds bump it so windows don't wrap);
// CLOCKWORK_SHM_AUDIO_FRAMES can override the frame count directly to express
// sub-second rings that the integer seconds knob cannot.
// ── The taps ────────────────────────────────────────────────────────────────
// Two slots, clockwork's, written inside every tick at the device edge:
//   OUT — what left for the device this block, after the guest and after
//         clockwork's own routing (port sinks, plugin-track sends).
//   IN  — what arrived from the device this block, as the guest saw it.
// The same on every host: the native callback, the worklet's quantum, a
// DAW's process. A client always has the master mix, and the input, with no
// guest cooperation, through the AUDIO_TAPS entry of the arena table
// (clockwork_arena.h). Each slot carries `channels` live device channels, up
// to CLOCKWORK_TAP_CHANNELS (memory_profile.h); `enabled` says the tap is
// live (the device has that direction); the cursor is monotonic and a
// reader catches up losslessly from its own position (shm_audio_buffer_reader).
//
// `holds` remains in the header for readers of the previous protocol, in
// which a guest wrote slot 0 while a client held it. Clockwork neither
// counts nor gates on it: the taps flow from boot.
inline constexpr uint32_t SHM_AUDIO_SLOTS       = 2;
inline constexpr uint32_t SHM_AUDIO_OUT_SLOT    = 0;
inline constexpr uint32_t SHM_AUDIO_IN_SLOT     = 1;
inline constexpr uint32_t MAX_SHM_AUDIO_BUFFERS = SHM_AUDIO_SLOTS;     // the old name
inline constexpr uint32_t SHM_AUDIO_MASTER_SLOT = SHM_AUDIO_OUT_SLOT;  // the old name

inline constexpr uint32_t SHM_AUDIO_SAMPLE_RATE = CLOCKWORK_SHM_AUDIO_SAMPLE_RATE;
inline constexpr uint32_t SHM_AUDIO_SECONDS     = CLOCKWORK_SHM_AUDIO_SECONDS;
inline constexpr uint32_t SHM_AUDIO_FRAMES      = CLOCKWORK_SHM_AUDIO_FRAMES;
inline constexpr uint32_t SHM_AUDIO_CHANNELS    = CLOCKWORK_TAP_CHANNELS;  // the ceiling a slot's ring is sized for
// Per-slot header bytes (atomics + sizes + 64-bit write_position +
// padding). Guaranteed 32 by the static_assert on offsetof(..., data).
inline constexpr uint32_t SHM_AUDIO_HEADER_SIZE = 32;

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

    // Pad to 32 bytes (header) so data starts at a 16-aligned offset.
    uint32_t              _reserved[2];

    // Interleaved float ring [ch0_f0, ch1_f0, ch0_f1, ch1_f1, ...].
    // Wraps modulo capacity_frames.
    float                 data[SHM_AUDIO_FRAMES * SHM_AUDIO_CHANNELS];
};

static_assert(std::is_trivially_destructible<shm_audio_buffer>::value,
              "shm_audio_buffer must be trivially destructible (lives in shm)");
static_assert(offsetof(shm_audio_buffer, data) == 32,
              "shm_audio_buffer header must be 32 bytes (data 16-aligned)");

inline constexpr uint32_t SHM_AUDIO_SLOT_SIZE  = sizeof(shm_audio_buffer);
inline constexpr uint32_t SHM_AUDIO_TOTAL_SIZE =
    MAX_SHM_AUDIO_BUFFERS * SHM_AUDIO_SLOT_SIZE;

// ──── Producer side ─────────────────────────────────────────────────────
//
// Activate a slot at a given sample rate / channel count. Returns false
// if the slot pointer is null. After activate(), call write*() per audio
// block; deactivate() to stop writes.

class shm_audio_buffer_writer {
public:
    shm_audio_buffer_writer() = default;
    explicit shm_audio_buffer_writer(shm_audio_buffer* buf) : _buf(buf) {}

    bool valid() const { return _buf != nullptr; }

    // Format the slot and mark it live. `channels` must be <= SHM_AUDIO_CHANNELS;
    // `capacity_frames` must be <= SHM_AUDIO_FRAMES (the inline ring size).
    // The cursor starts again: a reader's position is relative to this.
    bool activate(uint32_t channels, uint32_t sample_rate,
                  uint32_t capacity_frames) {
        if (!_buf) return false;
        if (channels == 0 || channels > SHM_AUDIO_CHANNELS) return false;
        if (capacity_frames == 0 || capacity_frames > SHM_AUDIO_FRAMES) return false;
        _buf->channels = channels;
        _buf->sample_rate = sample_rate;
        _buf->capacity_frames = capacity_frames;
        _buf->write_position.store(0, std::memory_order_relaxed);
        memset(_buf->data, 0, sizeof(_buf->data));
        _buf->enabled.store(1, std::memory_order_release);
        return true;
    }

    void write(const float* const* channel_data, uint32_t num_frames) {
        if (!_buf || num_frames == 0) return;
        uint64_t pos = _buf->write_position.load(std::memory_order_relaxed);
        uint32_t cap = _buf->capacity_frames;
        uint32_t channels = _buf->channels;
        uint32_t slot = static_cast<uint32_t>(pos % cap);
        uint32_t first = (cap - slot < num_frames) ? (cap - slot) : num_frames;
        float* dst = _buf->data + slot * channels;
        for (uint32_t f = 0; f < first; ++f) {
            for (uint32_t c = 0; c < channels; ++c)
                dst[f * channels + c] = channel_data[c][f];
        }
        if (first < num_frames) {
            uint32_t wrap = num_frames - first;
            dst = _buf->data;
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
        memcpy(_buf->data + slot * channels, data,
               static_cast<size_t>(first) * channels * sizeof(float));
        if (first < num_frames) {
            memcpy(_buf->data, data + first * channels,
                   static_cast<size_t>(num_frames - first) * channels * sizeof(float));
        }
        _buf->write_position.store(pos + num_frames, std::memory_order_release);
    }

    shm_audio_buffer* slot() const { return _buf; }

private:
    shm_audio_buffer* _buf = nullptr;
};

// ──── Consumer side ─────────────────────────────────────────────────────
//
// Reads new frames since the last call. Tracks read position locally;
// the writer's authoritative position is in the shm. If the reader falls
// more than capacity_frames behind, pull() reports a gap and resyncs.

class shm_audio_buffer_reader {
public:
    shm_audio_buffer_reader() = default;
    // keepalive pins the mapping the slot pointer reaches into (see
    // shm_segment_client): reader copies survive the client that
    // minted them. Engine-side readers pass nothing — their arena is static.
    // `max_frames` is the ring capacity the slot's ENGINE allocated; a
    // cross-process reader passes the engine's published value rather than
    // this build's SHM_AUDIO_FRAMES.
    explicit shm_audio_buffer_reader(shm_audio_buffer* buf,
                                     std::shared_ptr<const void> keepalive = nullptr,
                                     uint32_t max_frames = SHM_AUDIO_FRAMES)
        : _buf(buf), _keepalive(std::move(keepalive)),
          _max_frames(max_frames == 0 ? SHM_AUDIO_FRAMES : max_frames) {}

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
        if (channels == 0 || channels > SHM_AUDIO_CHANNELS) channels = SHM_AUDIO_CHANNELS;

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
        memcpy(out, _buf->data + slot * channels,
               static_cast<size_t>(first) * channels * sizeof(float));
        if (first < to_read) {
            memcpy(out + first * channels, _buf->data,
                   static_cast<size_t>(to_read - first) * channels * sizeof(float));
        }
        _last_read_pos += to_read;
        return to_read;
    }

    uint64_t last_read_position() const { return _last_read_pos; }

private:
    shm_audio_buffer* _buf = nullptr;
    std::shared_ptr<const void> _keepalive;
    uint32_t      _max_frames = SHM_AUDIO_FRAMES;
    uint64_t      _last_read_pos = 0;
};

} // namespace detail_shm_audio

using detail_shm_audio::shm_audio_buffer;
using detail_shm_audio::shm_audio_buffer_writer;
using detail_shm_audio::shm_audio_buffer_reader;
using detail_shm_audio::SHM_AUDIO_SAMPLE_RATE;
using detail_shm_audio::SHM_AUDIO_SECONDS;
using detail_shm_audio::SHM_AUDIO_CHANNELS;
using detail_shm_audio::SHM_AUDIO_FRAMES;
using detail_shm_audio::SHM_AUDIO_HEADER_SIZE;
using detail_shm_audio::SHM_AUDIO_SLOT_SIZE;
using detail_shm_audio::SHM_AUDIO_TOTAL_SIZE;
using detail_shm_audio::SHM_AUDIO_MASTER_SLOT;
using detail_shm_audio::MAX_SHM_AUDIO_BUFFERS;
using detail_shm_audio::SHM_AUDIO_SLOTS;
using detail_shm_audio::SHM_AUDIO_OUT_SLOT;
using detail_shm_audio::SHM_AUDIO_IN_SLOT;

// Process-global pointer to the slot array. Assigned once during
// audio_processor init and read-only after it. This is how a DSP reaches its
// slots: an extern is the whole of the arrangement, so nothing has to be
// threaded through the boundary for a region clockwork only publishes.
extern shm_audio_buffer* g_shm_audio_buffers;
