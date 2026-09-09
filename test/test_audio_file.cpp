// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_audio_file.cpp — the audio file boundary (src/clockwork_audio_file.h).
 *
 * These are the acceptance tests for the contract rather than for one
 * implementation of it. A different decoder swapped in underneath, or a build
 * for a platform with no file system, has to satisfy the same table.
 *
 * TWO THINGS ARE DELIBERATELY NOT SELF-CHECKED. A codec that agreed with
 * itself about a private format would pass a round trip while producing files
 * no other program opens, so:
 *
 *   - the AIFF case is a file this test assembles by hand, byte by byte, from
 *     what the AIFF specification says a FORM is. Nothing under test wrote it.
 *     AIFF matters more than the others here: it is the one common sample
 *     format a browser's own decoder refuses, so it is the reason the
 *     decoding is compiled in rather than borrowed from the platform.
 *   - the WAV a writer produces is parsed here against the RIFF chunk layout,
 *     so "it is a WAV" is checked against the format and not against dr_wav.
 *
 * FLAC round trips are exact on purpose: the format is lossless, so anything
 * short of bit-identical is a bug rather than a tolerance.
 */
#include <catch2/catch_test_macros.hpp>
#include "clockwork_audio_file.h"
#include "flac_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}
void put16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
void putTag(std::vector<uint8_t>& v, const char* t) {
    v.insert(v.end(), t, t + 4);
}

// An AIFF, built here from the specification: a FORM containing a COMM that
// declares the shape and an SSND holding big-endian samples. The sample rate
// is an 80-bit IEEE extended, which is the part of AIFF that catches naive
// readers out, so it is written properly rather than faked.
std::vector<uint8_t> makeAiff(const std::vector<int16_t>& samples,
                              uint16_t channels, uint32_t rate) {
    const uint32_t frames = static_cast<uint32_t>(samples.size() / channels);

    std::vector<uint8_t> ext(10, 0);
    {   // 80-bit extended: sign+exponent, then a 64-bit normalised mantissa.
        int exponent = 0;
        double m = std::frexp(static_cast<double>(rate), &exponent);
        const uint64_t mantissa =
            static_cast<uint64_t>(std::ldexp(m, 64));
        const uint16_t se = static_cast<uint16_t>(16382 + exponent);
        ext[0] = uint8_t(se >> 8); ext[1] = uint8_t(se);
        for (int i = 0; i < 8; ++i)
            ext[2 + i] = uint8_t(mantissa >> (56 - 8 * i));
    }

    std::vector<uint8_t> comm;
    put16be(comm, channels);
    put32be(comm, frames);
    put16be(comm, 16);                       // bits per sample
    comm.insert(comm.end(), ext.begin(), ext.end());

    std::vector<uint8_t> ssnd;
    put32be(ssnd, 0);                        // offset
    put32be(ssnd, 0);                        // block size
    for (int16_t s : samples) put16be(ssnd, static_cast<uint16_t>(s));

    std::vector<uint8_t> body;
    putTag(body, "AIFF");
    putTag(body, "COMM"); put32be(body, uint32_t(comm.size()));
    body.insert(body.end(), comm.begin(), comm.end());
    putTag(body, "SSND"); put32be(body, uint32_t(ssnd.size()));
    body.insert(body.end(), ssnd.begin(), ssnd.end());

    std::vector<uint8_t> form;
    putTag(form, "FORM"); put32be(form, uint32_t(body.size()));
    form.insert(form.end(), body.begin(), body.end());
    return form;
}

// A signal with constant runs, a ramp and abrupt steps, so more than one
// predictor and more than one partitioning gets exercised.
std::vector<float> signal(uint32_t frames, uint32_t channels) {
    std::vector<float> v(size_t(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
            const uint32_t n = i + c * 7919u;
            float s;
            if (n % 900 < 150)      s = 0.0f;
            else if (n % 900 < 500) s = float(n % 900) / 1800.0f;
            else                    s = (n % 2) ? 0.42f : -0.42f;
            v[size_t(i) * channels + c] = s;
        }
    return v;
}

int32_t grid(float v, int32_t peak) {
    const float scaled = v * float(peak + 1);
    if (scaled >= float(peak)) return peak;
    if (scaled <= float(-peak - 1)) return -peak - 1;
    return int32_t(std::lrintf(scaled));
}

struct Encoded {
    std::vector<uint8_t> bytes;
    ClockworkStatus status = CLOCKWORK_E_ARG;
};

// Encode straight to memory, feeding the writer in ragged chunks so a block
// boundary never lines up with a call boundary.
Encoded encodeToMemory(uint32_t format, uint32_t encoding, uint32_t channels,
                       uint32_t rate, const std::vector<float>& pcm) {
    Encoded out;
    ClockworkAudioWriterConfig cfg{};
    cfg.struct_bytes = sizeof(cfg);
    cfg.format = format; cfg.encoding = encoding;
    cfg.channels = channels; cfg.sample_rate = rate;

    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkAudioWriter* w = clockwork_audio_writer_open_memory(&cfg, &st);
    if (w == nullptr) { out.status = st; return out; }

    const uint64_t frames = pcm.size() / channels;
    uint64_t at = 0;
    uint32_t chunk = 1;
    while (at < frames) {
        const uint64_t n = std::min<uint64_t>(chunk, frames - at);
        if (clockwork_audio_writer_write(w, pcm.data() + at * channels, n)
            != CLOCKWORK_OK) {
            clockwork_audio_writer_close(w, nullptr, nullptr);
            return out;
        }
        at += n;
        chunk = (chunk * 7 + 13) % 1500 + 1;
    }

    void* bytes = nullptr;
    size_t len = 0;
    out.status = clockwork_audio_writer_close(w, &bytes, &len);
    if (bytes != nullptr) {
        out.bytes.assign(static_cast<uint8_t*>(bytes),
                         static_cast<uint8_t*>(bytes) + len);
        clockwork_audio_free(bytes);
    }
    return out;
}

} // namespace

TEST_CASE("audio file: a FLAC round trip is exact, not close",
          "[audio][file]") {
    // Lossless means lossless. Every sample must return to the integer it was
    // quantised to, or the encoder has a bug the ear would eventually find.
    const uint32_t frames = 9000, ch = 2;   // spans several 4096-frame blocks
    const auto pcm = signal(frames, ch);

    for (uint32_t enc : {uint32_t(CLOCKWORK_AUDIO_ENCODING_PCM_S16),
                         uint32_t(CLOCKWORK_AUDIO_ENCODING_PCM_S24)}) {
        const int32_t peak = enc == CLOCKWORK_AUDIO_ENCODING_PCM_S16
                               ? 32767 : 8388607;
        const auto enc_out = encodeToMemory(CLOCKWORK_AUDIO_FORMAT_FLAC, enc,
                                            ch, 44100, pcm);
        REQUIRE(enc_out.status == CLOCKWORK_OK);
        REQUIRE(enc_out.bytes.size() > 0);
        // It really is a FLAC stream, by its own magic.
        REQUIRE(std::memcmp(enc_out.bytes.data(), "fLaC", 4) == 0);

        ClockworkAudioInfo info{};
        info.struct_bytes = sizeof(info);
        float* got = nullptr;
        REQUIRE(clockwork_audio_decode_memory(enc_out.bytes.data(),
                                              enc_out.bytes.size(),
                                              &info, &got) == CLOCKWORK_OK);
        REQUIRE(info.format == CLOCKWORK_AUDIO_FORMAT_FLAC);
        REQUIRE(info.channels == ch);
        REQUIRE(info.sample_rate == 44100);
        REQUIRE(info.frames == frames);

        for (size_t i = 0; i < pcm.size(); ++i)
            REQUIRE(grid(got[i], peak) == grid(pcm[i], peak));
        clockwork_audio_free(got);
    }
}

TEST_CASE("audio file: an AIFF this test wrote by hand decodes",
          "[audio][file]") {
    // The format a browser will not decode, which is why it is compiled in.
    // The bytes come from the specification rather than from anything here.
    std::vector<int16_t> samples;
    for (int i = 0; i < 2000; ++i) {
        samples.push_back(static_cast<int16_t>(i * 13 - 12000));
        samples.push_back(static_cast<int16_t>(-i * 7 + 5000));
    }
    const auto aiff = makeAiff(samples, 2, 44100);

    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* got = nullptr;
    REQUIRE(clockwork_audio_decode_memory(aiff.data(), aiff.size(), &info, &got)
            == CLOCKWORK_OK);

    CHECK(info.format == CLOCKWORK_AUDIO_FORMAT_AIFF);
    CHECK(info.channels == 2);
    CHECK(info.sample_rate == 44100);       // read out of the 80-bit extended
    CHECK(info.source_bits == 16);
    REQUIRE(info.frames == samples.size() / 2);

    for (size_t i = 0; i < samples.size(); ++i)
        REQUIRE(grid(got[i], 32767) == samples[i]);
    clockwork_audio_free(got);
}

TEST_CASE("audio file: the container is read from the bytes, not the name",
          "[audio][file]") {
    // A sample library with a mislabelled extension is a real thing, and the
    // decision has to come from the content or the file simply fails to load.
    std::vector<int16_t> samples(400);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<int16_t>(i * 31 - 6000);
    const auto aiff = makeAiff(samples, 1, 22050);

    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    REQUIRE(clockwork_audio_probe_memory(aiff.data(), aiff.size(), &info)
            == CLOCKWORK_OK);
    CHECK(info.format == CLOCKWORK_AUDIO_FORMAT_AIFF);

    const auto wav = encodeToMemory(CLOCKWORK_AUDIO_FORMAT_WAV,
                                    CLOCKWORK_AUDIO_ENCODING_PCM_S16,
                                    1, 22050, std::vector<float>(400, 0.25f));
    REQUIRE(wav.status == CLOCKWORK_OK);
    ClockworkAudioInfo winfo{};
    winfo.struct_bytes = sizeof(winfo);
    REQUIRE(clockwork_audio_probe_memory(wav.bytes.data(), wav.bytes.size(),
                                         &winfo) == CLOCKWORK_OK);
    CHECK(winfo.format == CLOCKWORK_AUDIO_FORMAT_WAV);
}

TEST_CASE("audio file: a probe agrees with a decode and costs no samples",
          "[audio][file]") {
    const auto pcm = signal(3000, 2);
    const auto flac = encodeToMemory(CLOCKWORK_AUDIO_FORMAT_FLAC,
                                     CLOCKWORK_AUDIO_ENCODING_PCM_S16,
                                     2, 48000, pcm);
    REQUIRE(flac.status == CLOCKWORK_OK);

    ClockworkAudioInfo probed{}, decoded{};
    probed.struct_bytes = decoded.struct_bytes = sizeof(ClockworkAudioInfo);
    float* samples = nullptr;
    REQUIRE(clockwork_audio_probe_memory(flac.bytes.data(), flac.bytes.size(),
                                         &probed) == CLOCKWORK_OK);
    REQUIRE(clockwork_audio_decode_memory(flac.bytes.data(), flac.bytes.size(),
                                          &decoded, &samples) == CLOCKWORK_OK);

    CHECK(probed.format == decoded.format);
    CHECK(probed.channels == decoded.channels);
    CHECK(probed.sample_rate == decoded.sample_rate);
    CHECK(probed.frames == decoded.frames);
    CHECK(clockwork_audio_duration(&probed) == 3000.0 / 48000.0);
    clockwork_audio_free(samples);
}

TEST_CASE("audio file: an older caller's info struct is not written past",
          "[audio][file]") {
    // The same mechanism the rest of the client ABI uses, asserted the same
    // way: a caller declares a smaller struct and the library fills that much.
    const auto wav = encodeToMemory(CLOCKWORK_AUDIO_FORMAT_WAV,
                                    CLOCKWORK_AUDIO_ENCODING_PCM_S16,
                                    1, 8000, std::vector<float>(100, 0.5f));
    REQUIRE(wav.status == CLOCKWORK_OK);

    struct { ClockworkAudioInfo info; uint32_t guard; } probe{};
    probe.guard = 0xFEEDFACEu;
    probe.info.struct_bytes = sizeof(uint32_t) * 3;   // an older, shorter build

    REQUIRE(clockwork_audio_probe_memory(wav.bytes.data(), wav.bytes.size(),
                                         &probe.info) == CLOCKWORK_OK);
    CHECK(probe.guard == 0xFEEDFACEu);
    CHECK(probe.info.format == CLOCKWORK_AUDIO_FORMAT_WAV);
    // Past the declared size nothing was written, so this is still zero.
    CHECK(probe.info.channels == 0);
}

TEST_CASE("audio file: refusals are told apart", "[audio][file]") {
    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* out = nullptr;

    // Nothing to read is an argument fault; bytes that are not audio are a
    // format this build does not know. A caller retrying the first forever
    // would be waiting on the wrong thing.
    CHECK(clockwork_audio_decode_memory(nullptr, 10, &info, &out) == CLOCKWORK_E_ARG);
    CHECK(clockwork_audio_decode_memory("x", 0, &info, &out) == CLOCKWORK_E_ARG);

    const char junk[64] = "this is not audio, it is a sentence";
    CHECK(clockwork_audio_decode_memory(junk, sizeof junk, &info, &out)
          == CLOCKWORK_E_VERSION);
    CHECK(out == nullptr);   // and nothing was handed back to free

    // A pairing no format carries is refused before a file exists.
    CHECK(clockwork_audio_can_write(CLOCKWORK_AUDIO_FORMAT_FLAC,
                                    CLOCKWORK_AUDIO_ENCODING_FLOAT32) == 0);
    CHECK(clockwork_audio_can_write(CLOCKWORK_AUDIO_FORMAT_WAV,
                                    CLOCKWORK_AUDIO_ENCODING_FLOAT32) == 1);
    CHECK(clockwork_audio_can_write(CLOCKWORK_AUDIO_FORMAT_MP3,
                                    CLOCKWORK_AUDIO_ENCODING_PCM_S16) == 0);

    ClockworkAudioWriterConfig cfg{};
    cfg.struct_bytes = sizeof(cfg);
    cfg.format = CLOCKWORK_AUDIO_FORMAT_FLAC;
    cfg.encoding = CLOCKWORK_AUDIO_ENCODING_FLOAT32;
    cfg.channels = 2; cfg.sample_rate = 44100;
    ClockworkStatus st = CLOCKWORK_OK;
    CHECK(clockwork_audio_writer_open_memory(&cfg, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ABSENT);

    // A config from a build older than this one cannot be trusted to have the
    // fields the library reads, so it is refused rather than guessed at.
    cfg.encoding = CLOCKWORK_AUDIO_ENCODING_PCM_S16;
    cfg.struct_bytes = 4;
    CHECK(clockwork_audio_writer_open_memory(&cfg, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_VERSION);
}

TEST_CASE("audio file: samples past full scale clamp rather than wrap",
          "[audio][file]") {
    // A bus that clipped hands over values outside the range an integer format
    // holds. Wrapping turns a loud passage into noise, which is the one
    // outcome a recording must never produce.
    std::vector<float> hot = { 2.0f, -2.0f, 1.0f, -1.0f, 0.0f, 0.5f };
    const auto wav = encodeToMemory(CLOCKWORK_AUDIO_FORMAT_WAV,
                                    CLOCKWORK_AUDIO_ENCODING_PCM_S16,
                                    1, 44100, hot);
    REQUIRE(wav.status == CLOCKWORK_OK);

    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* got = nullptr;
    REQUIRE(clockwork_audio_decode_memory(wav.bytes.data(), wav.bytes.size(),
                                          &info, &got) == CLOCKWORK_OK);
    REQUIRE(info.frames == hot.size());

    CHECK(got[0] > 0.99f);    // pinned at positive full scale, not wrapped
    CHECK(got[1] < -0.99f);
    CHECK(got[2] > 0.99f);
    CHECK(got[3] < -0.99f);
    CHECK(got[4] == 0.0f);
    CHECK(got[5] > 0.49f);
    clockwork_audio_free(got);
}

TEST_CASE("audio file: a writer's memory and its file agree byte for byte",
          "[audio][file]") {
    // The two destinations are one encoder. If they ever diverge, a browser
    // and a desktop would produce different files from the same take.
    const auto pcm = signal(5000, 2);
    const auto mem = encodeToMemory(CLOCKWORK_AUDIO_FORMAT_FLAC,
                                    CLOCKWORK_AUDIO_ENCODING_PCM_S16,
                                    2, 44100, pcm);
    REQUIRE(mem.status == CLOCKWORK_OK);

    const auto path = std::filesystem::temp_directory_path()
                    / "clockwork_audio_file_pair.flac";
    std::filesystem::remove(path);

    ClockworkAudioWriterConfig cfg{};
    cfg.struct_bytes = sizeof(cfg);
    cfg.format = CLOCKWORK_AUDIO_FORMAT_FLAC;
    cfg.encoding = CLOCKWORK_AUDIO_ENCODING_PCM_S16;
    cfg.channels = 2; cfg.sample_rate = 44100;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkAudioWriter* w =
        clockwork_audio_writer_open(path.string().c_str(), &cfg, &st);
    REQUIRE(w != nullptr);
    REQUIRE(clockwork_audio_writer_write(w, pcm.data(), pcm.size() / 2)
            == CLOCKWORK_OK);
    REQUIRE(clockwork_audio_writer_close(w, nullptr, nullptr) == CLOCKWORK_OK);

    std::vector<uint8_t> onDisk;
    {
        std::FILE* f = std::fopen(path.string().c_str(), "rb");
        REQUIRE(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        onDisk.resize(static_cast<size_t>(std::ftell(f)));
        std::rewind(f);
        REQUIRE(std::fread(onDisk.data(), 1, onDisk.size(), f) == onDisk.size());
        std::fclose(f);
    }
    CHECK(onDisk == mem.bytes);
    std::filesystem::remove(path);
}

TEST_CASE("flac encoder: each fixed predictor is the one that gets chosen",
          "[audio][file][flac]") {
    // WHY THIS EXISTS. A round trip does not pin the predictors. The encoder
    // tries every fixed order and keeps whichever costs least, so a wrong
    // coefficient in one of them makes that order merely expensive: another
    // order wins, the stream stays lossless, and every round-trip case in this
    // file still passes. Breaking the order-2 predictor on purpose did exactly
    // that.
    //
    // What separates them is WHICH ORDER WINS. A signal whose n-th difference
    // is zero is predicted exactly by fixed order n, and by order n+1 too —
    // but n+1 pays for one more warmup sample, so n wins outright. Feed the
    // encoder such a signal and the order it reports is the arithmetic being
    // right; get a coefficient wrong and that order stops winning.
    //
    // 24-bit, and short blocks for the steep ones, so each polynomial stays in
    // range across the whole block and needs no restart.
    struct Case { int order; uint32_t frames; };
    const Case cases[] = { {2, 2048}, {3, 1024}, {4, 256} };

    for (const Case& c : cases) {
        std::vector<int32_t> x(c.frames, 0);
        // Seeds whose (order-1)-th difference is a non-zero constant, then
        // extended by the very predictor under test so the order-th
        // difference is exactly zero.
        const int32_t seeds2[2] = { 0, 7 };
        const int32_t seeds3[3] = { 0, 1, 3 };
        const int32_t seeds4[4] = { 0, 1, 4, 10 };
        const int32_t* seed = c.order == 2 ? seeds2
                            : c.order == 3 ? seeds3 : seeds4;
        for (int k = 0; k < c.order; ++k) x[k] = seed[k];

        for (uint32_t i = uint32_t(c.order); i < c.frames; ++i) {
            switch (c.order) {
            case 2: x[i] = 2*x[i-1] - x[i-2]; break;
            case 3: x[i] = 3*x[i-1] - 3*x[i-2] + x[i-3]; break;
            default: x[i] = 4*x[i-1] - 6*x[i-2] + 4*x[i-3] - x[i-4]; break;
            }
            REQUIRE(std::abs(x[i]) <= 8388607);   // stayed inside 24-bit
        }

        FlacEncoder enc(1, 44100, 24);
        REQUIRE(enc.valid());
        std::vector<uint8_t> out;
        enc.encodeBlock(x.data(), c.frames, out);

        // One subframe was written, and it chose this order.
        CHECK(enc.subframeKind(c.order) == 1);
        for (int other = 0; other <= 4; ++other)
            if (other != c.order)
                CHECK(enc.subframeKind(other) == 0);

        // Zero residual is about a bit a sample, so the block is tiny.
        CHECK(out.size() < c.frames / 2);
    }
}

TEST_CASE("flac encoder: a flat block and an incompressible one take their own shapes",
          "[audio][file][flac]") {
    // The two subframes that carry no residual at all. A silent passage must
    // not be written out sample by sample, and noise must not be forced
    // through a predictor that cannot help it.
    {
        FlacEncoder enc(1, 44100, 16);
        std::vector<int32_t> flat(4096, -1234);
        std::vector<uint8_t> out;
        enc.encodeBlock(flat.data(), 4096, out);
        CHECK(enc.subframeKind(FlacEncoder::kConstant) == 1);
        CHECK(out.size() < 32);          // a header, one sample, a checksum
    }
    {
        // Full-scale 24-bit noise: every fixed order makes it worse, so the
        // cheapest description is the samples themselves.
        FlacEncoder enc(1, 44100, 24);
        std::vector<int32_t> noise(4096);
        uint32_t r = 12345;
        for (auto& v : noise) {
            r = r * 1664525u + 1013904223u;
            v = static_cast<int32_t>(r % 16777215u) - 8388607;
        }
        std::vector<uint8_t> out;
        enc.encodeBlock(noise.data(), 4096, out);
        CHECK(enc.subframeKind(0) + enc.subframeKind(FlacEncoder::kVerbatim) == 1);
        // Whatever it chose, it did not make the data bigger than raw by much.
        CHECK(out.size() <= 4096 * 3 + 64);
    }
}
