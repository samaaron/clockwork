// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_audio_file.cpp — the file reading, the versioned info and the
 * writers behind clockwork_audio_file.h.
 *
 * Decoding is Rust: rust/clockwork-audio-decode, reached through
 * clockwork_audio_decode.h. It recognises each container from its own first
 * bytes rather than from the name it was saved under, so a .wav that is
 * really an AIFF still loads, and it hands its frames into a buffer this
 * side grows with realloc, so what a caller frees with clockwork_audio_free
 * is a plain malloc block. The file entry points read the bytes and hand
 * them to the memory entry points: one path for a disk, a socket and a
 * browser's ArrayBuffer.
 *
 * Writing is this file's own: a WAV header and its patched sizes, and the
 * FLAC encoder in flac_encoder.h.
 */
#include "clockwork_audio_file.h"
#include "clockwork_audio_decode.h"
#include "owner.h"
#include "flac_encoder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>   // std::malloc, std::realloc, std::free
#include <cstring>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include "clockwork_path.h"   // a path is UTF-8; Windows' own calls take UTF-16
#else
#include <fcntl.h>
#include <unistd.h>
#endif
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

ClockworkAudioInfo toInfo(const ClockworkAudioDecodeInfo& d) {
    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    info.format      = d.format;
    info.encoding    = d.encoding;
    info.channels    = d.channels;
    info.sample_rate = d.sample_rate;
    info.source_bits = d.source_bits;
    info.frames      = d.frames;
    return info;
}

// ── The decode limit ─────────────────────────────────────────────────────────
#if defined(__EMSCRIPTEN__)
constexpr uint64_t kDefaultDecodeLimit = 256ull * 1024 * 1024;
#else
constexpr uint64_t kDefaultDecodeLimit = 1024ull * 1024 * 1024;
#endif
std::atomic<uint64_t> gDecodeLimit{kDefaultDecodeLimit};

// ── Reading a whole file ─────────────────────────────────────────────────────
// Decoding needs every byte, so the file entry points read the file and then
// take the memory path.
#if defined(CLOCKWORK_AUDIO_NO_STDIO)
ClockworkStatus slurp(const char*, std::vector<uint8_t>&) {
    return CLOCKWORK_E_ABSENT;
}
#else
// A path is UTF-8 on every platform (clockwork_path.h). Windows' narrow
// fopen would read it in the ANSI code page and open a different file.
std::FILE* openToRead(const char* path) {
#if defined(_WIN32)
    return ::_wfopen(clockwork_path::to_wide(path).c_str(), L"rb");
#else
    return std::fopen(path, "rb");
#endif
}

ClockworkStatus slurp(const char* path, std::vector<uint8_t>& out) {
    if (path == nullptr || *path == '\0') return CLOCKWORK_E_ARG;
    const std::unique_ptr<std::FILE, int (*)(std::FILE*)> f(openToRead(path), &std::fclose);
    if (f == nullptr) return CLOCKWORK_E_NOT_FOUND;

    if (std::fseek(f.get(), 0, SEEK_END) != 0) return CLOCKWORK_E_ARG;
    const long size = std::ftell(f.get());
    if (size < 0) return CLOCKWORK_E_ARG;
    // No file decodes to less than its own size by more than its header, so
    // one past the decode limit is refused here, before a byte of it is
    // read: the limit bounds what a file costs, not only what it yields.
    if (static_cast<uint64_t>(size) > gDecodeLimit.load(std::memory_order_relaxed)) return CLOCKWORK_E_TOO_BIG;
    if (std::fseek(f.get(), 0, SEEK_SET) != 0) return CLOCKWORK_E_ARG;

    out.resize(static_cast<size_t>(size));
    const size_t got = size > 0 ? std::fread(out.data(), 1, out.size(), f.get()) : 0;
    if (got != out.size()) return CLOCKWORK_E_ARG;
    return CLOCKWORK_OK;
}
#endif

// realloc, in the shape the decoder grows its output through. The block is
// the caller's from the first byte, released with clockwork_audio_free.
void* grow(void* block, size_t bytes) {
    return std::realloc(block, bytes);   // NOLINT(cppcoreguidelines-no-malloc): the contract, see clockwork_audio_free
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
    if (data == nullptr || bytes == 0) return CLOCKWORK_E_ARG;
    ClockworkAudioDecodeInfo d{};
    const auto st = static_cast<ClockworkStatus>(
        clockwork_audio_decode_probe(static_cast<const uint8_t*>(data), bytes, &d));
    if (st != CLOCKWORK_OK) return st;
    return fillInfo(info, toInfo(d));
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
    if (data == nullptr || bytes == 0) return CLOCKWORK_E_ARG;

    ClockworkAudioDecodeInfo d{};
    gsl::owner<float*> samples = nullptr;
    const auto st = static_cast<ClockworkStatus>(clockwork_audio_decode_run(
        static_cast<const uint8_t*>(data), bytes, clockwork_audio_decode_limit(),
        &grow, &d, &samples));
    if (st != CLOCKWORK_OK) { std::free(samples); return st; }   // NOLINT(cppcoreguidelines-no-malloc): what was grown so far

    const ClockworkStatus fst = fillInfo(info, toInfo(d));
    if (fst != CLOCKWORK_OK) { std::free(samples); return fst; }   // NOLINT(cppcoreguidelines-no-malloc)
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

void clockwork_audio_free(gsl::owner<void*> interleaved) {   // the caller's, given back here
    std::free(interleaved);   // NOLINT(cppcoreguidelines-no-malloc): what grow hands out
}

void clockwork_audio_set_decode_limit(uint64_t max_bytes) {
    gDecodeLimit.store(max_bytes == 0 ? kDefaultDecodeLimit : max_bytes, std::memory_order_relaxed);
}

uint64_t clockwork_audio_decode_limit(void) {
    return gDecodeLimit.load(std::memory_order_relaxed);
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

    // Where the bytes go: a memory writer keeps them, a path writer's file is
    // `fp`, and a WAV and a FLAC are fed the same way.
    std::vector<uint8_t> memory;
    gsl::owner<std::FILE*> fp = nullptr;

    // WAV: the sample bytes written so far, for the sizes patched at close.
    uint64_t wavDataBytes = 0;
    std::vector<uint8_t> scratch;

    // FLAC: blocks are gathered and encoded as they fill.
    std::unique_ptr<FlacEncoder> flac;
    std::vector<int32_t> block;    // interleaved, up to kBlockSize frames
    uint32_t blockFrames = 0;
    std::vector<uint8_t> frameBytes;

    // A writer dropped before close — open() failing after it took a file,
    // or a caller that never closed — gives back what it holds.
    ~ClockworkAudioWriter() {
        if (fp) std::fclose(fp);
    }
    ClockworkAudioWriter() = default;
    ClockworkAudioWriter(const ClockworkAudioWriter&) = delete;
    ClockworkAudioWriter& operator=(const ClockworkAudioWriter&) = delete;
};

namespace {

// Bytes to wherever this writer sends them.
bool emit(ClockworkAudioWriter* w, const uint8_t* data, size_t len) {
    if (len == 0) return true;
    if (w->toMemory) {
        w->memory.insert(w->memory.end(), data, data + len);
        return true;
    }
    return std::fwrite(data, 1, len, w->fp) == len;
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFFu);
    put16(v, x >> 16);
}

constexpr size_t kWavRiffSizeAt = 4;
constexpr size_t kWavDataSizeAt = 40;
constexpr size_t kWavHeaderBytes = 44;

uint32_t wavBits(const ClockworkAudioWriter* w) {
    return w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16 ? 16u
         : w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24 ? 24u : 32u;
}

// The 44-byte RIFF/WAVE header, its sizes zero until close patches them.
std::vector<uint8_t> wavHeader(const ClockworkAudioWriter* w) {
    const uint32_t bits = wavBits(w);
    const uint32_t blockAlign = w->channels * bits / 8;
    std::vector<uint8_t> h;
    h.reserve(kWavHeaderBytes);
    h.insert(h.end(), {'R', 'I', 'F', 'F'}); put32(h, 0);
    h.insert(h.end(), {'W', 'A', 'V', 'E'});
    h.insert(h.end(), {'f', 'm', 't', ' '}); put32(h, 16);
    put16(h, w->encoding == CLOCKWORK_AUDIO_ENCODING_FLOAT32 ? 3u : 1u);   // IEEE float or PCM
    put16(h, w->channels);
    put32(h, w->sampleRate);
    put32(h, w->sampleRate * blockAlign);
    put16(h, blockAlign);
    put16(h, bits);
    h.insert(h.end(), {'d', 'a', 't', 'a'}); put32(h, 0);
    return h;
}

// The two sizes a RIFF carries, which a WAV past 4 GB cannot: that is what
// RF64 is for, and this writer does not write one.
bool wavSizes(const ClockworkAudioWriter* w, uint32_t& riff, uint32_t& data) {
    if (w->wavDataBytes > 0xFFFFFFFFull - (kWavHeaderBytes - 8)) return false;
    data = static_cast<uint32_t>(w->wavDataBytes);
    riff = static_cast<uint32_t>(w->wavDataBytes + kWavHeaderBytes - 8);
    return true;
}

bool flushFlacBlock(ClockworkAudioWriter* w) {
    if (w->blockFrames == 0) return true;
    w->frameBytes.clear();
    w->flac->encodeBlock(w->block.data(), w->blockFrames, w->frameBytes);
    w->blockFrames = 0;
    if (w->frameBytes.empty()) return false;
    return emit(w, w->frameBytes.data(), w->frameBytes.size());
}

// Hand a vector's contents to a caller that will release them with
// clockwork_audio_free, which is free().
bool handOver(const std::vector<uint8_t>& src, void** bytes, size_t* len) {
    gsl::owner<void*> p = std::malloc(std::max<size_t>(src.size(), 1));   // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory): the caller's, freed by clockwork_audio_free
    if (p == nullptr) return false;
    if (!src.empty()) std::memcpy(p, src.data(), src.size());
    *bytes = p;
    *len = src.size();
    return true;
}

// Everything an open does before it knows where the bytes are going.
gsl::owner<ClockworkAudioWriter*> validateAndMake(const ClockworkAudioWriterConfig* config,
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

    gsl::owner<ClockworkAudioWriter*> w = new ClockworkAudioWriter();
    w->format     = config->format;
    w->encoding   = config->encoding;
    w->channels   = config->channels;
    w->sampleRate = config->sample_rate;
    return w;
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

// The header of either format, as the first bytes out.
bool startStream(ClockworkAudioWriter* w) {
    if (w->format == CLOCKWORK_AUDIO_FORMAT_WAV) {
        const auto h = wavHeader(w);
        return emit(w, h.data(), h.size());
    }
    if (!startFlac(w)) return false;
    const auto hdr = w->flac->header();
    return emit(w, hdr.data(), hdr.size());
}

#if !defined(CLOCKWORK_AUDIO_NO_STDIO)
// A file of the user's, created readable by all and written by its owner
// (0644, under the umask), said here rather than left to a library's default
// of writable by all. Windows has no mode to give: a new file takes its ACL
// from its folder, and _S_IWRITE only keeps it from being made read-only.
// There the UTF-8 path goes through the wide call, as openToRead's does.
gsl::owner<std::FILE*> createFile(const char* path) {
#if defined(_WIN32)
    const int fd = ::_wopen(clockwork_path::to_wide(path).c_str(),
                            _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT,
                            _S_IREAD | _S_IWRITE);
    if (fd < 0) return nullptr;
    const gsl::owner<std::FILE*> f{::_fdopen(fd, "wb")};   // the descriptor is the stream's now
    if (f == nullptr) ::_close(fd);
    return f;
#else
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return nullptr;
    const gsl::owner<std::FILE*> f{::fdopen(fd, "wb")};   // the descriptor is the stream's now
    if (f == nullptr) ::close(fd);
    return f;
#endif
}
#endif

// Write `bytes` over the stream at `at`: into the memory, or through a seek.
bool patch(ClockworkAudioWriter* w, size_t at, const uint8_t* bytes, size_t len) {
    if (w->toMemory) {
        if (w->memory.size() < at + len) return false;
        std::memcpy(w->memory.data() + at, bytes, len);
        return true;
    }
    return std::fseek(w->fp, static_cast<long>(at), SEEK_SET) == 0
        && std::fwrite(bytes, 1, len, w->fp) == len;
}

} // namespace

gsl::owner<ClockworkAudioWriter*> clockwork_audio_writer_open(
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

    w->fp = createFile(path);
    if (w->fp == nullptr) return fail(CLOCKWORK_E_PERM);   // nothing created to clean up
    if (!startStream(w.get())) return fail(CLOCKWORK_E_PERM);   // the writer closes its file

    if (status != nullptr) *status = CLOCKWORK_OK;
    return w.release();
#endif
}

gsl::owner<ClockworkAudioWriter*> clockwork_audio_writer_open_memory(
    const ClockworkAudioWriterConfig* config, ClockworkStatus* status) {

    const auto fail = [status](ClockworkStatus s) -> ClockworkAudioWriter* {
        if (status != nullptr) *status = s;
        return nullptr;
    };

    std::unique_ptr<ClockworkAudioWriter> w(validateAndMake(config, status));
    if (w == nullptr) return nullptr;
    w->toMemory = true;
    if (!startStream(w.get())) return fail(CLOCKWORK_E_ARG);

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
        const uint8_t* src = reinterpret_cast<const uint8_t*>(interleaved);
        size_t len = samples * sizeof(float);
        if (w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S16) {
            w->scratch.resize(samples * 2);
            for (size_t i = 0; i < samples; ++i) {
                const auto v = static_cast<uint32_t>(quantize(interleaved[i], 32767));
                w->scratch[i * 2 + 0] = static_cast<uint8_t>(v & 0xFFu);
                w->scratch[i * 2 + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
            }
            src = w->scratch.data();
            len = w->scratch.size();
        } else if (w->encoding == CLOCKWORK_AUDIO_ENCODING_PCM_S24) {
            w->scratch.resize(samples * 3);
            for (size_t i = 0; i < samples; ++i) {
                const auto v = static_cast<uint32_t>(quantize(interleaved[i], 8388607));
                w->scratch[i * 3 + 0] = static_cast<uint8_t>(v & 0xFFu);
                w->scratch[i * 3 + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
                w->scratch[i * 3 + 2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
            }
            src = w->scratch.data();
            len = w->scratch.size();
        }
        if (!emit(w, src, len)) {
            w->status = CLOCKWORK_E_PERM;
            return w->status;
        }
        w->wavDataBytes += len;
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

    const auto note = [w](ClockworkStatus s) { if (w->status == CLOCKWORK_OK) w->status = s; };

    if (w->format == CLOCKWORK_AUDIO_FORMAT_WAV) {
        // The RIFF and data sizes, now the totals are known.
        uint32_t riff = 0, data = 0;
        if (!wavSizes(w, riff, data)) {
            note(CLOCKWORK_E_TOO_BIG);
        } else {
            std::vector<uint8_t> four;
            put32(four, riff);
            if (!patch(w, kWavRiffSizeAt, four.data(), 4)) note(CLOCKWORK_E_PERM);
            four.clear();
            put32(four, data);
            if (!patch(w, kWavDataSizeAt, four.data(), 4)) note(CLOCKWORK_E_PERM);
        }
    } else {
        if (!flushFlacBlock(w)) note(CLOCKWORK_E_PERM);
        // The stream info written at open could not know the totals. Now it
        // can, so it is written again over the placeholder.
        const auto tail = w->flac->finalStreamInfo();
        if (!patch(w, FlacEncoder::kStreamInfoOffset, tail.data(), tail.size()))
            note(w->toMemory ? CLOCKWORK_E_NOMEM : CLOCKWORK_E_PERM);
    }

    if (w->toMemory) {
        if (out_bytes != nullptr && out_bytes_len != nullptr
            && !handOver(w->memory, out_bytes, out_bytes_len))
            note(CLOCKWORK_E_NOMEM);
        return w->status;
    }

    if (w->fp != nullptr) {
        if (std::fclose(w->fp) != 0) note(CLOCKWORK_E_PERM);
        w->fp = nullptr;
    }
    return w->status;
}
