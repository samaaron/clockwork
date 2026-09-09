// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_audio_file.cpp — the decoders and the writers behind
 * clockwork_audio_file.h.
 *
 * Each container is recognised from its own first bytes rather than from the
 * name it was saved under, so a .wav that is really an AIFF still loads. The
 * file entry points read the bytes and hand them to the memory entry points:
 * one implementation of every format, and the path a browser takes is the path
 * a desktop takes.
 */
#include "clockwork_audio_file.h"
#include "flac_encoder.h"

// A build with no file system still reads and writes audio: the memory
// entry points are the whole API there, and the path ones report absent.
#if defined(CLOCKWORK_AUDIO_NO_STDIO)
  #define DR_WAV_NO_STDIO
  #define DR_FLAC_NO_STDIO
  #define DR_MP3_NO_STDIO
#endif

#define DR_WAV_IMPLEMENTATION
#define DRWAV_NO_STDIO_WARNINGS
#include "vendor/dr_libs/dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "vendor/dr_libs/dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_libs/dr_mp3.h"

// stb_vorbis is compiled as its own translation unit; this pulls in only its
// declarations.
#define STB_VORBIS_HEADER_ONLY
#include "vendor/stb/stb_vorbis.c"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

// ── Filling a versioned struct ───────────────────────────────────────────────
// The caller's struct_bytes says how much of it exists; anything past that
// belongs to a newer header than the caller was built against.
ClockworkStatus fillInfo(ClockworkAudioInfo* out, const ClockworkAudioInfo& full) {
    if (out == nullptr) return CLOCKWORK_E_ARG;
    const uint32_t want = out->struct_bytes;
    if (want < sizeof(uint32_t) * 2) return CLOCKWORK_E_ARG;

    ClockworkAudioInfo copy = full;
    copy.struct_bytes = want;
    std::memcpy(out, &copy, std::min<size_t>(want, sizeof(ClockworkAudioInfo)));
    return CLOCKWORK_OK;
}

bool startsWith(const uint8_t* d, size_t n, size_t at, const char* tag) {
    const size_t len = std::strlen(tag);
    return n >= at + len && std::memcmp(d + at, tag, len) == 0;
}

// The first sixteen bytes of a Wave64 file: the RIFF GUID.
const uint8_t kW64Riff[16] = {
    0x72,0x69,0x66,0x66, 0x2E,0x91, 0xCF,0x11, 0xA5,0xD6,
    0x28,0xDB,0x04,0xC1,0x00,0x00
};

ClockworkAudioFormat sniff(const uint8_t* d, size_t n) {
    if (d == nullptr || n < 4) return CLOCKWORK_AUDIO_FORMAT_UNKNOWN;

    if (startsWith(d, n, 0, "fLaC")) return CLOCKWORK_AUDIO_FORMAT_FLAC;
    if (startsWith(d, n, 0, "OggS")) return CLOCKWORK_AUDIO_FORMAT_OGG;
    if (startsWith(d, n, 0, "RF64")) return CLOCKWORK_AUDIO_FORMAT_RF64;
    if (n >= 16 && std::memcmp(d, kW64Riff, 16) == 0)
        return CLOCKWORK_AUDIO_FORMAT_W64;
    if ((startsWith(d, n, 0, "RIFF") || startsWith(d, n, 0, "RIFX"))
        && startsWith(d, n, 8, "WAVE"))
        return CLOCKWORK_AUDIO_FORMAT_WAV;
    if (startsWith(d, n, 0, "FORM")
        && (startsWith(d, n, 8, "AIFF") || startsWith(d, n, 8, "AIFC")))
        return CLOCKWORK_AUDIO_FORMAT_AIFF;

    // MP3 carries no container magic. An ID3 tag, or a frame sync with a
    // layer III version that is not the reserved one, is as much as there is
    // to go on — and it is what every player uses.
    if (startsWith(d, n, 0, "ID3")) return CLOCKWORK_AUDIO_FORMAT_MP3;
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0
        && (d[1] & 0x18) != 0x08 && (d[1] & 0x06) == 0x02)
        return CLOCKWORK_AUDIO_FORMAT_MP3;

    return CLOCKWORK_AUDIO_FORMAT_UNKNOWN;
}

uint32_t encodingOfWav(const drwav& wav) {
    if (wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT)
        return CLOCKWORK_AUDIO_ENCODING_FLOAT32;
    switch (wav.bitsPerSample) {
    case 16: return CLOCKWORK_AUDIO_ENCODING_PCM_S16;
    case 24: return CLOCKWORK_AUDIO_ENCODING_PCM_S24;
    case 32: return CLOCKWORK_AUDIO_ENCODING_PCM_S32;
    default: return CLOCKWORK_AUDIO_ENCODING_UNKNOWN;
    }
}

// ── Reading a whole file ─────────────────────────────────────────────────────
// Decoding needs every byte, so the file entry points read the file and then
// take the memory path. One decoder per format, reached the same way from a
// disk, a socket or a browser's ArrayBuffer.
#if defined(CLOCKWORK_AUDIO_NO_STDIO)
ClockworkStatus slurp(const char*, std::vector<uint8_t>&) {
    return CLOCKWORK_E_ABSENT;
}
#else
ClockworkStatus slurp(const char* path, std::vector<uint8_t>& out) {
    if (path == nullptr || *path == '\0') return CLOCKWORK_E_ARG;
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return CLOCKWORK_E_NOT_FOUND;

    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return CLOCKWORK_E_ARG; }
    const long size = std::ftell(f);
    if (size < 0) { std::fclose(f); return CLOCKWORK_E_ARG; }
    std::rewind(f);

    out.resize(static_cast<size_t>(size));
    const size_t got = size > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
    std::fclose(f);
    if (got != out.size()) return CLOCKWORK_E_ARG;
    return CLOCKWORK_OK;
}
#endif

// ── Per-format probes and decodes ────────────────────────────────────────────
// `samples` null means probe only: headers are read, the audio is not.

ClockworkStatus readWav(const uint8_t* d, size_t n, ClockworkAudioFormat fmt,
                        ClockworkAudioInfo& info, float** samples) {
    drwav wav;
    if (!drwav_init_memory(&wav, d, n, nullptr)) return CLOCKWORK_E_ARG;

    info.format      = fmt;
    info.encoding    = encodingOfWav(wav);
    info.channels    = wav.channels;
    info.sample_rate = wav.sampleRate;
    info.source_bits = wav.bitsPerSample;
    info.frames      = wav.totalPCMFrameCount;

    ClockworkStatus st = CLOCKWORK_OK;
    if (samples != nullptr) {
        const size_t count = static_cast<size_t>(info.frames) * info.channels;
        float* buf = count > 0 ? static_cast<float*>(std::malloc(count * sizeof(float)))
                               : static_cast<float*>(std::malloc(1));
        if (buf == nullptr) st = CLOCKWORK_E_NOMEM;
        else if (drwav_read_pcm_frames_f32(&wav, info.frames, buf) != info.frames) {
            std::free(buf);
            st = CLOCKWORK_E_ARG;
        } else {
            *samples = buf;
        }
    }
    drwav_uninit(&wav);
    return st;
}

ClockworkStatus readFlac(const uint8_t* d, size_t n,
                         ClockworkAudioInfo& info, float** samples) {
    drflac* flac = drflac_open_memory(d, n, nullptr);
    if (flac == nullptr) return CLOCKWORK_E_ARG;

    info.format      = CLOCKWORK_AUDIO_FORMAT_FLAC;
    info.channels    = flac->channels;
    info.sample_rate = flac->sampleRate;
    info.source_bits = flac->bitsPerSample;
    info.frames      = flac->totalPCMFrameCount;
    info.encoding    = flac->bitsPerSample == 16 ? CLOCKWORK_AUDIO_ENCODING_PCM_S16
                     : flac->bitsPerSample == 24 ? CLOCKWORK_AUDIO_ENCODING_PCM_S24
                                                 : CLOCKWORK_AUDIO_ENCODING_PCM_S32;

    ClockworkStatus st = CLOCKWORK_OK;
    if (samples != nullptr) {
        const size_t count = static_cast<size_t>(info.frames) * info.channels;
        float* buf = static_cast<float*>(std::malloc(std::max<size_t>(count, 1) * sizeof(float)));
        if (buf == nullptr) st = CLOCKWORK_E_NOMEM;
        else if (drflac_read_pcm_frames_f32(flac, info.frames, buf) != info.frames) {
            std::free(buf);
            st = CLOCKWORK_E_ARG;
        } else {
            *samples = buf;
        }
    }
    drflac_close(flac);
    return st;
}

ClockworkStatus readMp3(const uint8_t* d, size_t n,
                        ClockworkAudioInfo& info, float** samples) {
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, d, n, nullptr)) return CLOCKWORK_E_ARG;

    const drmp3_uint64 frames = drmp3_get_pcm_frame_count(&mp3);
    info.format      = CLOCKWORK_AUDIO_FORMAT_MP3;
    info.encoding    = CLOCKWORK_AUDIO_ENCODING_COMPRESSED;
    info.channels    = mp3.channels;
    info.sample_rate = mp3.sampleRate;
    info.source_bits = 0;
    info.frames      = frames;

    ClockworkStatus st = CLOCKWORK_OK;
    if (samples != nullptr) {
        const size_t count = static_cast<size_t>(frames) * info.channels;
        float* buf = static_cast<float*>(std::malloc(std::max<size_t>(count, 1) * sizeof(float)));
        if (buf == nullptr) {
            st = CLOCKWORK_E_NOMEM;
        } else {
            drmp3_seek_to_pcm_frame(&mp3, 0);
            const drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, frames, buf);
            // A truncated stream decodes to fewer frames than the scan
            // promised; the silence keeps the buffer the length the caller was
            // told it is.
            if (got < frames)
                std::memset(buf + got * info.channels, 0,
                            static_cast<size_t>(frames - got) * info.channels * sizeof(float));
            *samples = buf;
        }
    }
    drmp3_uninit(&mp3);
    return st;
}

ClockworkStatus readOgg(const uint8_t* d, size_t n,
                        ClockworkAudioInfo& info, float** samples) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_memory(d, static_cast<int>(n), &err, nullptr);
    if (v == nullptr) return CLOCKWORK_E_ARG;

    const stb_vorbis_info vi = stb_vorbis_get_info(v);
    const unsigned int frames = stb_vorbis_stream_length_in_samples(v);

    info.format      = CLOCKWORK_AUDIO_FORMAT_OGG;
    info.encoding    = CLOCKWORK_AUDIO_ENCODING_COMPRESSED;
    info.channels    = static_cast<uint32_t>(vi.channels);
    info.sample_rate = static_cast<uint32_t>(vi.sample_rate);
    info.source_bits = 0;
    info.frames      = frames;

    ClockworkStatus st = CLOCKWORK_OK;
    if (samples != nullptr) {
        const size_t count = static_cast<size_t>(frames) * info.channels;
        float* buf = static_cast<float*>(std::malloc(std::max<size_t>(count, 1) * sizeof(float)));
        if (buf == nullptr) {
            st = CLOCKWORK_E_NOMEM;
        } else {
            const int got = stb_vorbis_get_samples_float_interleaved(
                v, vi.channels, buf, static_cast<int>(count));
            if (got >= 0 && static_cast<unsigned>(got) < frames)
                std::memset(buf + static_cast<size_t>(got) * info.channels, 0,
                            (frames - got) * info.channels * sizeof(float));
            *samples = buf;
        }
    }
    stb_vorbis_close(v);
    return st;
}

ClockworkStatus readAny(const void* data, size_t bytes,
                        ClockworkAudioInfo& info, float** samples) {
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (d == nullptr || bytes == 0) return CLOCKWORK_E_ARG;

    switch (sniff(d, bytes)) {
    case CLOCKWORK_AUDIO_FORMAT_WAV:
    case CLOCKWORK_AUDIO_FORMAT_AIFF:
    case CLOCKWORK_AUDIO_FORMAT_W64:
    case CLOCKWORK_AUDIO_FORMAT_RF64:
        return readWav(d, bytes, sniff(d, bytes), info, samples);
    case CLOCKWORK_AUDIO_FORMAT_FLAC: return readFlac(d, bytes, info, samples);
    case CLOCKWORK_AUDIO_FORMAT_OGG:  return readOgg(d, bytes, info, samples);
    case CLOCKWORK_AUDIO_FORMAT_MP3:  return readMp3(d, bytes, info, samples);
    default:                          return CLOCKWORK_E_VERSION;
    }
}

// ── Float to the integer the file wants ──────────────────────────────────────
// A bus that clipped hands over samples outside the range an integer format
// can hold; they are clamped, because the alternative is a wrap that turns a
// loud passage into noise.
inline int32_t quantize(float v, int32_t peak) {
    const float scaled = v * static_cast<float>(peak + 1);
    if (scaled >= static_cast<float>(peak)) return peak;
    if (scaled <= static_cast<float>(-peak - 1)) return -peak - 1;
    return static_cast<int32_t>(std::lrintf(scaled));
}

} // namespace

// ── Public surface ───────────────────────────────────────────────────────────

double clockwork_audio_duration(const ClockworkAudioInfo* info) {
    if (info == nullptr || info->sample_rate == 0) return 0.0;
    return static_cast<double>(info->frames) / info->sample_rate;
}

ClockworkStatus clockwork_audio_probe_memory(const void* data, size_t bytes,
                                             ClockworkAudioInfo* info) {
    if (info == nullptr) return CLOCKWORK_E_ARG;
    ClockworkAudioInfo full{};
    full.struct_bytes = sizeof(full);
    const ClockworkStatus st = readAny(data, bytes, full, nullptr);
    if (st != CLOCKWORK_OK) return st;
    return fillInfo(info, full);
}

ClockworkStatus clockwork_audio_probe_file(const char* path,
                                           ClockworkAudioInfo* info) {
    std::vector<uint8_t> bytes;
    const ClockworkStatus st = slurp(path, bytes);
    if (st != CLOCKWORK_OK) return st;
    return clockwork_audio_probe_memory(bytes.data(), bytes.size(), info);
}

ClockworkStatus clockwork_audio_decode_memory(const void* data, size_t bytes,
                                              ClockworkAudioInfo* info,
                                              float** out_interleaved) {
    if (info == nullptr || out_interleaved == nullptr) return CLOCKWORK_E_ARG;
    ClockworkAudioInfo full{};
    full.struct_bytes = sizeof(full);
    float* samples = nullptr;
    const ClockworkStatus st = readAny(data, bytes, full, &samples);
    if (st != CLOCKWORK_OK) return st;

    const ClockworkStatus fst = fillInfo(info, full);
    if (fst != CLOCKWORK_OK) { std::free(samples); return fst; }
    *out_interleaved = samples;
    return CLOCKWORK_OK;
}

ClockworkStatus clockwork_audio_decode_file(const char* path,
                                            ClockworkAudioInfo* info,
                                            float** out_interleaved) {
    std::vector<uint8_t> bytes;
    const ClockworkStatus st = slurp(path, bytes);
    if (st != CLOCKWORK_OK) return st;
    return clockwork_audio_decode_memory(bytes.data(), bytes.size(),
                                         info, out_interleaved);
}

void clockwork_audio_free(void* interleaved) {
    std::free(interleaved);
}

int clockwork_audio_can_write(uint32_t format, uint32_t encoding) {
    if (format == CLOCKWORK_AUDIO_FORMAT_WAV)
        return encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16
            || encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24
            || encoding == CLOCKWORK_AUDIO_ENCODING_FLOAT32;
    if (format == CLOCKWORK_AUDIO_FORMAT_FLAC)
        return encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16
            || encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24;
    return 0;
}

// ── The writer ───────────────────────────────────────────────────────────────

struct ClockworkAudioWriter {
    uint32_t format = 0;
    uint32_t encoding = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t frames = 0;
    bool     toMemory = false;
    ClockworkStatus status = CLOCKWORK_OK;

    // WAV. dr_wav owns the growing buffer for a memory writer and hands it
    // back at uninit; for a path writer it owns the file.
    drwav  wav{};
    bool   wavOpen = false;
    void*  wavMemory = nullptr;
    size_t wavMemoryLen = 0;
    std::vector<uint8_t> scratch;

    // FLAC. A path writer streams frames out as they are encoded; a memory
    // writer keeps them, since the stream info at the front is patched at the
    // end either way.
    std::FILE* fp = nullptr;
    std::unique_ptr<FlacEncoder> flac;
    std::vector<int32_t> block;    // interleaved, up to kBlockSize frames
    uint32_t blockFrames = 0;
    std::vector<uint8_t> frameBytes;
    std::vector<uint8_t> memory;
};

namespace {

bool flushFlacBlock(ClockworkAudioWriter* w) {
    if (w->blockFrames == 0) return true;
    w->frameBytes.clear();
    w->flac->encodeBlock(w->block.data(), w->blockFrames, w->frameBytes);
    w->blockFrames = 0;
    if (w->frameBytes.empty()) return false;
    if (w->toMemory) {
        w->memory.insert(w->memory.end(), w->frameBytes.begin(), w->frameBytes.end());
        return true;
    }
    return std::fwrite(w->frameBytes.data(), 1, w->frameBytes.size(), w->fp)
           == w->frameBytes.size();
}

// Hand a vector's contents to a caller that will release them with
// clockwork_audio_free, which is free().
bool handOver(const std::vector<uint8_t>& src, void** bytes, size_t* len) {
    void* p = std::malloc(std::max<size_t>(src.size(), 1));
    if (p == nullptr) return false;
    if (!src.empty()) std::memcpy(p, src.data(), src.size());
    *bytes = p;
    *len = src.size();
    return true;
}

} // namespace

namespace {

// Everything an open does before it knows where the bytes are going.
ClockworkAudioWriter* validateAndMake(const ClockworkAudioWriterConfig* config,
                                      ClockworkStatus* status) {
    const auto fail = [status](ClockworkStatus s) -> ClockworkAudioWriter* {
        if (status != nullptr) *status = s;
        return nullptr;
    };
    if (config == nullptr) return fail(CLOCKWORK_E_ARG);
    if (config->struct_bytes < sizeof(ClockworkAudioWriterConfig))
        return fail(CLOCKWORK_E_VERSION);
    if (config->channels == 0 || config->sample_rate == 0)
        return fail(CLOCKWORK_E_ARG);
    if (!clockwork_audio_can_write(config->format, config->encoding))
        return fail(CLOCKWORK_E_ABSENT);

    auto* w = new ClockworkAudioWriter();
    w->format     = config->format;
    w->encoding   = config->encoding;
    w->channels   = config->channels;
    w->sampleRate = config->sample_rate;
    return w;
}

drwav_data_format wavFormat(const ClockworkAudioWriter* w) {
    drwav_data_format fmt{};
    fmt.container     = drwav_container_riff;
    fmt.format        = w->encoding == CLOCKWORK_AUDIO_ENCODING_FLOAT32
                          ? DR_WAVE_FORMAT_IEEE_FLOAT : DR_WAVE_FORMAT_PCM;
    fmt.channels      = w->channels;
    fmt.sampleRate    = w->sampleRate;
    fmt.bitsPerSample = w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16 ? 16
                      : w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24 ? 24
                                                                        : 32;
    return fmt;
}

uint32_t flacBits(const ClockworkAudioWriter* w) {
    return w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16 ? 16u : 24u;
}

bool startFlac(ClockworkAudioWriter* w) {
    w->flac = std::make_unique<FlacEncoder>(w->channels, w->sampleRate,
                                            flacBits(w));
    if (!w->flac->valid()) return false;
    w->block.resize(static_cast<size_t>(FlacEncoder::kBlockSize) * w->channels);
    return true;
}

} // namespace

ClockworkAudioWriter* clockwork_audio_writer_open(
    const char* path, const ClockworkAudioWriterConfig* config,
    ClockworkStatus* status) {

    const auto fail = [status](ClockworkStatus s) -> ClockworkAudioWriter* {
        if (status != nullptr) *status = s;
        return nullptr;
    };

#if defined(CLOCKWORK_AUDIO_NO_STDIO)
    (void)path; (void)config;
    return fail(CLOCKWORK_E_ABSENT);
#else
    if (path == nullptr || *path == '\0') return fail(CLOCKWORK_E_ARG);

    std::unique_ptr<ClockworkAudioWriter> w(validateAndMake(config, status));
    if (w == nullptr) return nullptr;

    if (w->format == CLOCKWORK_AUDIO_FORMAT_WAV) {
        const drwav_data_format fmt = wavFormat(w.get());
        if (!drwav_init_file_write(&w->wav, path, &fmt, nullptr))
            return fail(CLOCKWORK_E_PERM);
        w->wavOpen = true;
    } else {
        if (!startFlac(w.get())) return fail(CLOCKWORK_E_ARG);

        w->fp = std::fopen(path, "wb");
        if (w->fp == nullptr) return fail(CLOCKWORK_E_PERM);

        const auto hdr = w->flac->header();
        if (std::fwrite(hdr.data(), 1, hdr.size(), w->fp) != hdr.size()) {
            std::fclose(w->fp);
            w->fp = nullptr;
            return fail(CLOCKWORK_E_PERM);
        }
    }

    if (status != nullptr) *status = CLOCKWORK_OK;
    return w.release();
#endif
}

ClockworkAudioWriter* clockwork_audio_writer_open_memory(
    const ClockworkAudioWriterConfig* config, ClockworkStatus* status) {

    const auto fail = [status](ClockworkStatus s) -> ClockworkAudioWriter* {
        if (status != nullptr) *status = s;
        return nullptr;
    };

    std::unique_ptr<ClockworkAudioWriter> w(validateAndMake(config, status));
    if (w == nullptr) return nullptr;
    w->toMemory = true;

    if (w->format == CLOCKWORK_AUDIO_FORMAT_WAV) {
        const drwav_data_format fmt = wavFormat(w.get());
        if (!drwav_init_memory_write(&w->wav, &w->wavMemory, &w->wavMemoryLen,
                                     &fmt, nullptr))
            return fail(CLOCKWORK_E_NOMEM);
        w->wavOpen = true;
    } else {
        if (!startFlac(w.get())) return fail(CLOCKWORK_E_ARG);
        const auto hdr = w->flac->header();
        w->memory.assign(hdr.begin(), hdr.end());
    }

    if (status != nullptr) *status = CLOCKWORK_OK;
    return w.release();
}

ClockworkStatus clockwork_audio_writer_write(ClockworkAudioWriter* w,
                                             const float* interleaved,
                                             uint64_t frames) {
    if (w == nullptr || interleaved == nullptr) return CLOCKWORK_E_ARG;
    if (frames == 0) return CLOCKWORK_OK;
    if (w->status != CLOCKWORK_OK) return w->status;

    const size_t samples = static_cast<size_t>(frames) * w->channels;

    if (w->format == CLOCKWORK_AUDIO_FORMAT_WAV) {
        const void* src = interleaved;
        if (w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16) {
            w->scratch.resize(samples * 2);
            auto* p = reinterpret_cast<int16_t*>(w->scratch.data());
            for (size_t i = 0; i < samples; ++i)
                p[i] = static_cast<int16_t>(quantize(interleaved[i], 32767));
            src = w->scratch.data();
        } else if (w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24) {
            w->scratch.resize(samples * 3);
            uint8_t* p = w->scratch.data();
            for (size_t i = 0; i < samples; ++i) {
                const int32_t v = quantize(interleaved[i], 8388607);
                p[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
                p[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
                p[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            }
            src = w->scratch.data();
        }
        if (drwav_write_pcm_frames(&w->wav, frames, src) != frames) {
            w->status = CLOCKWORK_E_PERM;
            return w->status;
        }
        w->frames += frames;
        return CLOCKWORK_OK;
    }

    // FLAC is written a block at a time, so frames are gathered until there
    // are enough and the remainder waits for the next call or for close.
    const int32_t peak = w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16
                           ? 32767 : 8388607;
    uint64_t done = 0;
    while (done < frames) {
        const uint32_t room = FlacEncoder::kBlockSize - w->blockFrames;
        const uint32_t take = static_cast<uint32_t>(
            std::min<uint64_t>(room, frames - done));

        const float* in = interleaved + done * w->channels;
        int32_t* out = w->block.data()
                     + static_cast<size_t>(w->blockFrames) * w->channels;
        const size_t count = static_cast<size_t>(take) * w->channels;
        for (size_t i = 0; i < count; ++i)
            out[i] = quantize(in[i], peak);

        w->blockFrames += take;
        done += take;
        if (w->blockFrames == FlacEncoder::kBlockSize && !flushFlacBlock(w)) {
            w->status = CLOCKWORK_E_PERM;
            return w->status;
        }
    }
    w->frames += frames;
    return CLOCKWORK_OK;
}

uint64_t clockwork_audio_writer_frames(const ClockworkAudioWriter* w) {
    return w != nullptr ? w->frames : 0;
}

ClockworkStatus clockwork_audio_writer_close(ClockworkAudioWriter* w,
                                             void** out_bytes,
                                             size_t* out_bytes_len) {
    if (out_bytes != nullptr) *out_bytes = nullptr;
    if (out_bytes_len != nullptr) *out_bytes_len = 0;
    if (w == nullptr) return CLOCKWORK_OK;
    std::unique_ptr<ClockworkAudioWriter> owned(w);

    if (w->format == CLOCKWORK_AUDIO_FORMAT_WAV) {
        // dr_wav patches the RIFF sizes here, and for a memory writer this is
        // where the finished buffer appears.
        if (w->wavOpen) drwav_uninit(&w->wav);
        w->wavOpen = false;
        if (w->toMemory) {
            if (out_bytes != nullptr && out_bytes_len != nullptr) {
                *out_bytes = w->wavMemory;
                *out_bytes_len = w->wavMemoryLen;
            } else {
                std::free(w->wavMemory);
            }
            w->wavMemory = nullptr;
        }
        return w->status;
    }

    if (!w->toMemory && w->fp == nullptr) return w->status;

    if (!flushFlacBlock(w) && w->status == CLOCKWORK_OK)
        w->status = CLOCKWORK_E_PERM;

    // The stream info written at open could not know the totals. Now it can,
    // so it is written again over the placeholder.
    const auto tail = w->flac->finalStreamInfo();

    if (w->toMemory) {
        if (w->memory.size() >= FlacEncoder::kStreamInfoOffset + tail.size())
            std::memcpy(w->memory.data() + FlacEncoder::kStreamInfoOffset,
                        tail.data(), tail.size());
        else if (w->status == CLOCKWORK_OK)
            w->status = CLOCKWORK_E_NOMEM;

        if (out_bytes != nullptr && out_bytes_len != nullptr
            && !handOver(w->memory, out_bytes, out_bytes_len)
            && w->status == CLOCKWORK_OK)
            w->status = CLOCKWORK_E_NOMEM;
        return w->status;
    }

    if (std::fseek(w->fp, static_cast<long>(FlacEncoder::kStreamInfoOffset),
                   SEEK_SET) == 0) {
        if (std::fwrite(tail.data(), 1, tail.size(), w->fp) != tail.size()
            && w->status == CLOCKWORK_OK)
            w->status = CLOCKWORK_E_PERM;
    } else if (w->status == CLOCKWORK_OK) {
        w->status = CLOCKWORK_E_PERM;
    }

    if (std::fclose(w->fp) != 0 && w->status == CLOCKWORK_OK)
        w->status = CLOCKWORK_E_PERM;
    w->fp = nullptr;
    return w->status;
}
