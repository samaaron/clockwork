// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The audio writer (src/clockwork_audio_file.cpp, src/flac_encoder.cpp) takes
// whatever a user's own DSP produced: any channel count, any rate, samples
// that are NaN, infinite, denormal or far outside the unit range, in writes
// of any length. The input here is the whole of that: a header naming the
// writer's configuration and how the frames are chunked, then raw float
// samples. The writer may refuse a configuration at open; what it accepts it
// must write without fault, count exactly, and finish as a stream whose own
// header agrees with what was written.

#include "clockwork_audio_file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

struct Input {
    uint32_t format, encoding, channels, sampleRate;
    uint8_t  chunking, scale, repeat;
    const uint8_t* samples;   // raw floats, read with a copy: the header leaves them unaligned
    size_t count;
};

constexpr size_t kHeaderBytes = 13;

bool parse(const uint8_t* data, size_t size, Input& in) {
    if (size < kHeaderBytes) return false;
    in.format   = data[0];
    in.encoding = data[1];
    in.chunking = data[2];
    in.scale    = data[3];
    std::memcpy(&in.channels, data + 4, 4);
    std::memcpy(&in.sampleRate, data + 8, 4);
    in.repeat   = data[12];
    in.samples  = data + kHeaderBytes;
    in.count    = (size - kHeaderBytes) / sizeof(float);
    return true;
}

uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8); }

[[noreturn]] void fault() { __builtin_trap(); }
void must(bool ok) { if (!ok) fault(); }

// The stream's own account of itself, against what was written.
void checkWav(const uint8_t* b, size_t len, const Input& in, uint64_t frames) {
    must(len >= 44);
    must(std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVEfmt ", 8) == 0);
    const uint32_t bits = in.encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16 ? 16
                        : in.encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24 ? 24 : 32;
    const uint64_t dataBytes = frames * in.channels * (bits / 8);
    must(len == 44 + dataBytes);
    must(le32(b + 4) == len - 8);
    must(le16(b + 22) == in.channels);
    must(le32(b + 24) == in.sampleRate);
    must(le32(b + 28) == static_cast<uint64_t>(in.sampleRate) * in.channels * (bits / 8));   // byte rate
    must(le16(b + 32) == in.channels * (bits / 8));                                          // block align
    must(le16(b + 34) == bits);
    must(std::memcmp(b + 36, "data", 4) == 0 && le32(b + 40) == dataBytes);
}

void checkFlac(const uint8_t* b, size_t len, const Input& in, uint64_t frames) {
    must(len >= 8 + 34);
    must(std::memcmp(b, "fLaC", 4) == 0);
    const uint8_t* si = b + 8;   // STREAMINFO body
    // bytes 10..17: 20 bits of rate, 3 of channels-1, 5 of bits-1, 36 of total samples
    const uint64_t packed = (static_cast<uint64_t>(si[10]) << 56) | (static_cast<uint64_t>(si[11]) << 48)
                          | (static_cast<uint64_t>(si[12]) << 40) | (static_cast<uint64_t>(si[13]) << 32)
                          | (static_cast<uint64_t>(si[14]) << 24) | (static_cast<uint64_t>(si[15]) << 16)
                          | (static_cast<uint64_t>(si[16]) << 8)  |  static_cast<uint64_t>(si[17]);
    const uint32_t rate     = static_cast<uint32_t>(packed >> 44);
    const uint32_t channels = static_cast<uint32_t>((packed >> 41) & 0x7) + 1;
    const uint32_t bits     = static_cast<uint32_t>((packed >> 36) & 0x1F) + 1;
    const uint64_t total    = packed & 0xFFFFFFFFFull;
    must(rate == in.sampleRate);
    must(channels == in.channels);
    must(bits == (in.encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16 ? 16u : 24u));
    must(total == frames);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Input in{};
    if (!parse(data, size, in)) return 0;

    ClockworkAudioWriterConfig cfg{};
    cfg.struct_bytes = sizeof(cfg);
    cfg.format      = in.format;
    cfg.encoding    = in.encoding;
    cfg.channels    = in.channels;
    cfg.sample_rate = in.sampleRate;

    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkAudioWriter* w = clockwork_audio_writer_open_memory(&cfg, &st);
    if (w == nullptr) {
        must(st != CLOCKWORK_OK);
        return 0;
    }
    must(st == CLOCKWORK_OK);
    must(clockwork_audio_can_write(in.format, in.encoding) != 0);

    // The samples as the DSP left them, or scaled to the extremes a bus can
    // reach: far outside the unit range, or down among the denormals.
    const float scale = in.scale % 4 == 0 ? 1.f : in.scale % 4 == 1 ? 1e30f
                      : in.scale % 4 == 2 ? 1e-30f : -3.f;
    // The samples cycle up to eight times over, so a FLAC sees whole 4096-frame
    // blocks and not only the partial one that close encodes.
    float frame[4096];
    const uint64_t once   = in.channels == 0 ? 0 : in.count / in.channels;
    const uint64_t frames = once * (in.repeat % 8 + 1);
    uint64_t written = 0;
    uint32_t chunk = static_cast<uint32_t>(in.chunking) + 1;
    while (written < frames) {
        const uint32_t room = static_cast<uint32_t>(sizeof(frame) / sizeof(float) / in.channels);
        if (room == 0) break;   // more channels than a frame fits: the open was the test
        uint64_t n = chunk < room ? chunk : room;
        if (n > frames - written) n = frames - written;
        for (uint64_t i = 0; i < n * in.channels; ++i) {
            float v;
            const uint64_t at = ((written * in.channels) + i) % (once * in.channels);
            std::memcpy(&v, in.samples + at * sizeof(float), sizeof(float));
            frame[i] = v * scale;
        }
        must(clockwork_audio_writer_write(w, frame, n) == CLOCKWORK_OK);
        written += n;
        chunk = (chunk * 7 + 13) % 300 + 1;
    }
    must(clockwork_audio_writer_write(w, frame, 0) == CLOCKWORK_OK);   // an empty write is allowed
    must(clockwork_audio_writer_frames(w) == written);

    void* bytes = nullptr;
    size_t len = 0;
    must(clockwork_audio_writer_close(w, &bytes, &len) == CLOCKWORK_OK);
    must(bytes != nullptr);
    const auto* b = static_cast<const uint8_t*>(bytes);
    if (in.format == CLOCKWORK_AUDIO_FORMAT_WAV) checkWav(b, len, in, written);
    else checkFlac(b, len, in, written);
    clockwork_audio_free(bytes);
    return 0;
}
