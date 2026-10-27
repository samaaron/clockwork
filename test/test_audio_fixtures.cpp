// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_audio_fixtures.cpp — every format clockwork_audio_file.h promises,
 * decoded from files other encoders wrote (test/fixtures/audio, made by
 * make.py there with ffmpeg, lame and flac).
 *
 * The signal in every fixture is `sample` below, integer arithmetic shared
 * with make.py, so a lossless decode is compared sample for sample with no
 * reference. A lossy or companded fixture comes with ffmpeg's own decode of
 * it (`.f32`, raw interleaved float) and is compared within a tolerance that
 * two correct decoders stay well inside and a wrong one does not.
 *
 * The decoder is the widest door clockwork has to untrusted bytes: sample
 * files come from users, the web and other people's projects. So beyond the
 * formats, these cases pin what a hostile file can and cannot do: a header
 * that claims more than the file holds, a length past what memory could
 * hold, a file cut short, bytes that are not audio at all.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "clockwork_audio_file.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::filesystem::path kFixtures = CLOCKWORK_AUDIO_FIXTURES;

// How far a lossy decode may sit from ffmpeg's, of full scale. Two decoders
// of the same MP3 or Vorbis stream agree to the sample: the specifications
// define the output, and the only slack is arithmetic precision. Measured
// 2026-10-27 at 1.4e-7 over every lossy fixture, so this is a hundred times
// that, and a one-sample shift between the two would fail it by a mile, as
// it should.
constexpr float kLossyTolerance = 1e-5f;

// make.py's signal: runs, a ramp and steps, different per channel.
int32_t sample(uint32_t i, uint32_t c) {
    const uint32_t n = i + c * 7919u;
    if (n % 900 < 150) return 0;
    if (n % 900 < 500) return static_cast<int32_t>((n % 900) - 150) * 46 - 8000;
    return n % 2 == 0 ? 13797 : -13797;
}

std::vector<uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::vector<float> readReference(const std::string& fixture) {
    auto p = kFixtures / fixture;
    p.replace_extension(".f32");
    const auto bytes = readFile(p);
    std::vector<float> f(bytes.size() / sizeof(float));
    std::memcpy(f.data(), bytes.data(), f.size() * sizeof(float));
    return f;
}

struct Decoded {
    ClockworkStatus status = CLOCKWORK_E_ARG;
    ClockworkAudioInfo info{};
    std::vector<float> samples;
};

Decoded decodeBytes(const std::vector<uint8_t>& bytes) {
    Decoded d;
    d.info.struct_bytes = sizeof(d.info);
    float* out = nullptr;
    d.status = clockwork_audio_decode_memory(bytes.data(), bytes.size(), &d.info, &out);
    if (d.status == CLOCKWORK_OK) {
        REQUIRE(out != nullptr);
        d.samples.assign(out, out + static_cast<size_t>(d.info.frames) * d.info.channels);
        clockwork_audio_free(out);
    } else {
        REQUIRE(out == nullptr);
    }
    return d;
}

Decoded decodeFixture(const std::string& name) {
    return decodeBytes(readFile(kFixtures / name));
}

ClockworkAudioInfo probeBytes(const std::vector<uint8_t>& bytes, ClockworkStatus expect = CLOCKWORK_OK) {
    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    REQUIRE(clockwork_audio_probe_memory(bytes.data(), bytes.size(), &info) == expect);
    return info;
}

// The expected float of a lossless fixture: the 16-bit signal over full
// scale, or its top byte for an 8-bit file.
float expected(uint32_t i, uint32_t c, bool eightBit) {
    const int32_t v = sample(i, c);
    return eightBit ? static_cast<float>(v >> 8) / 128.0f
                    : static_cast<float>(v) / 32768.0f;
}

void checkLossless(const Decoded& d, uint32_t channels, uint32_t rate, uint32_t frames,
                   uint32_t format, uint32_t encoding, uint32_t bits, bool eightBit = false) {
    REQUIRE(d.status == CLOCKWORK_OK);
    CHECK(d.info.format == format);
    CHECK(d.info.encoding == encoding);
    CHECK(d.info.source_bits == bits);
    CHECK(d.info.channels == channels);
    CHECK(d.info.sample_rate == rate);
    REQUIRE(d.info.frames == frames);
    REQUIRE(d.samples.size() == static_cast<size_t>(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
            const float got = d.samples[static_cast<size_t>(i) * channels + c];
            const float want = expected(i, c, eightBit);
            if (got != want) {
                INFO("frame " << i << " channel " << c);
                REQUIRE(got == want);
            }
        }
}

// A lossy fixture against ffmpeg's decode of it: the same length, and every
// sample within `tolerance` of full scale.
void checkAgainstReference(const std::string& name, const Decoded& d, uint32_t channels,
                           uint32_t rate, uint32_t format, float tolerance) {
    REQUIRE(d.status == CLOCKWORK_OK);
    CHECK(d.info.format == format);
    CHECK(d.info.channels == channels);
    CHECK(d.info.sample_rate == rate);
    const auto ref = readReference(name);
    REQUIRE(ref.size() % channels == 0);
    INFO(name << ": ours " << d.info.frames << " frames, ffmpeg " << ref.size() / channels);
    REQUIRE(d.samples.size() == ref.size());
    float worst = 0.0f;
    size_t worstAt = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        const float diff = std::fabs(d.samples[i] - ref[i]);
        if (diff > worst) { worst = diff; worstAt = i; }
    }
    INFO("largest difference " << worst << " at sample " << worstAt);
    REQUIRE(worst <= tolerance);
}

// A RIFF/WAVE with a 16-bit stereo fmt and a data chunk whose declared size
// is `claimedDataBytes`, holding `actualFrames` frames of the signal.
std::vector<uint8_t> craftWav(uint32_t claimedDataBytes, uint32_t actualFrames) {
    std::vector<uint8_t> v;
    auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); };
    auto put16 = [&](uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    auto tag = [&](const char* t) { v.insert(v.end(), t, t + 4); };
    tag("RIFF"); put32(0xFFFFFFF0u); tag("WAVE");
    tag("fmt "); put32(16); put16(1); put16(2); put32(44100); put32(44100 * 4); put16(4); put16(16);
    tag("data"); put32(claimedDataBytes);
    for (uint32_t i = 0; i < actualFrames; ++i)
        for (uint32_t c = 0; c < 2; ++c) put16(static_cast<uint16_t>(sample(i, c)));
    return v;
}

} // namespace

// ── Every lossless format and encoding, sample for sample ───────────────────

TEST_CASE("audio fixtures: WAV in every PCM and float encoding", "[audio][file][fixtures]") {
    struct Row { const char* file; uint32_t encoding; uint32_t bits; bool eightBit; };
    const Row row = GENERATE(
        Row{"wav-u8.wav",  CLOCKWORK_AUDIO_ENCODING_PCM_8, 8, true},
        Row{"wav-s16.wav", CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16, false},
        Row{"wav-s24.wav", CLOCKWORK_AUDIO_ENCODING_PCM_S24, 24, false},
        Row{"wav-s32.wav", CLOCKWORK_AUDIO_ENCODING_PCM_S32, 32, false},
        Row{"wav-f32.wav", CLOCKWORK_AUDIO_ENCODING_FLOAT32, 32, false},
        Row{"wav-f64.wav", CLOCKWORK_AUDIO_ENCODING_FLOAT64, 64, false});
    INFO(row.file);
    checkLossless(decodeFixture(row.file), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_WAV, row.encoding, row.bits, row.eightBit);
}

TEST_CASE("audio fixtures: six channels, plain and extensible", "[audio][file][fixtures]") {
    const char* file = GENERATE("wav-6ch.wav", "wav-6ch-extensible.wav");
    INFO(file);
    checkLossless(decodeFixture(file), 6, 48000, 2400,
                  CLOCKWORK_AUDIO_FORMAT_WAV, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
}

TEST_CASE("audio fixtures: the 64-bit WAVs, RF64 and Wave64", "[audio][file][fixtures]") {
    checkLossless(decodeFixture("rf64-s16.wav"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_RF64, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
    checkLossless(decodeFixture("w64-s16.w64"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_W64, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
    checkLossless(decodeFixture("w64-s24.w64"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_W64, CLOCKWORK_AUDIO_ENCODING_PCM_S24, 24);
}

TEST_CASE("audio fixtures: AIFF and AIFC", "[audio][file][fixtures]") {
    checkLossless(decodeFixture("aiff-s8.aiff"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_AIFF, CLOCKWORK_AUDIO_ENCODING_PCM_8, 8, true);
    checkLossless(decodeFixture("aiff-s16.aiff"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_AIFF, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
    checkLossless(decodeFixture("aiff-s24.aiff"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_AIFF, CLOCKWORK_AUDIO_ENCODING_PCM_S24, 24);
    checkLossless(decodeFixture("aifc-f32.aiff"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_AIFF, CLOCKWORK_AUDIO_ENCODING_FLOAT32, 32);
    checkLossless(decodeFixture("aiff-6ch.aiff"), 6, 48000, 2400,
                  CLOCKWORK_AUDIO_FORMAT_AIFF, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
}

TEST_CASE("audio fixtures: FLAC at every depth, mono to six channels, 96 kHz", "[audio][file][fixtures]") {
    checkLossless(decodeFixture("flac-s8.flac"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_8, 8, true);
    checkLossless(decodeFixture("flac-s16.flac"), 2, 44100, 4410,
                  CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
    checkLossless(decodeFixture("flac-s24-96k.flac"), 2, 96000, 4800,
                  CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_S24, 24);
    checkLossless(decodeFixture("flac-mono.flac"), 1, 22050, 4410,
                  CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
    checkLossless(decodeFixture("flac-6ch.flac"), 6, 48000, 2400,
                  CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 16);
}

// ── Companded and lossy, against ffmpeg ─────────────────────────────────────

TEST_CASE("audio fixtures: A-law, mu-law and ADPCM decode as ffmpeg decodes them", "[audio][file][fixtures]") {
    // Companding is a table and ADPCM a fixed algorithm: two correct decoders
    // agree to the last bit of a 16-bit sample, so the tolerance is one step.
    const float step = 1.0f / 32768.0f;
    checkAgainstReference("wav-alaw.wav", decodeFixture("wav-alaw.wav"), 1, 8000,
                          CLOCKWORK_AUDIO_FORMAT_WAV, step);
    checkAgainstReference("wav-mulaw.wav", decodeFixture("wav-mulaw.wav"), 1, 8000,
                          CLOCKWORK_AUDIO_FORMAT_WAV, step);
    checkAgainstReference("wav-ima-adpcm.wav", decodeFixture("wav-ima-adpcm.wav"), 2, 44100,
                          CLOCKWORK_AUDIO_FORMAT_WAV, step);
    checkAgainstReference("wav-ms-adpcm.wav", decodeFixture("wav-ms-adpcm.wav"), 2, 44100,
                          CLOCKWORK_AUDIO_FORMAT_WAV, step);
}

TEST_CASE("audio fixtures: MP3 in every shape a player meets", "[audio][file][fixtures]") {
    // Constant and variable rate; with the LAME tag that says how much
    // encoder delay and padding to trim, and without it; MPEG-1, MPEG-2 and
    // MPEG-2.5; an ID3v2 tag in front and an ID3v1 tag behind.
    struct Row { const char* file; uint32_t channels; uint32_t rate; };
    const Row row = GENERATE(
        Row{"mp3-cbr.mp3", 2, 44100},
        Row{"mp3-vbr.mp3", 2, 44100},
        Row{"mp3-noxing.mp3", 2, 44100},
        Row{"mp3-id3v2.mp3", 2, 44100},
        Row{"mp3-id3v1.mp3", 2, 44100},
        Row{"mp3-22050-mono.mp3", 1, 22050},
        Row{"mp3-8000-mono.mp3", 1, 8000});
    INFO(row.file);
    const auto d = decodeFixture(row.file);
    CHECK(d.info.encoding == CLOCKWORK_AUDIO_ENCODING_COMPRESSED);
    CHECK(d.info.source_bits == 0);
    checkAgainstReference(row.file, d, row.channels, row.rate, CLOCKWORK_AUDIO_FORMAT_MP3, kLossyTolerance);
}

TEST_CASE("audio fixtures: Ogg Vorbis, stereo and mono", "[audio][file][fixtures]") {
    const auto st = decodeFixture("ogg-q3.ogg");
    CHECK(st.info.encoding == CLOCKWORK_AUDIO_ENCODING_COMPRESSED);
    checkAgainstReference("ogg-q3.ogg", st, 2, 44100, CLOCKWORK_AUDIO_FORMAT_OGG, kLossyTolerance);
    checkAgainstReference("ogg-mono.ogg", decodeFixture("ogg-mono.ogg"), 1, 22050,
                          CLOCKWORK_AUDIO_FORMAT_OGG, kLossyTolerance);
}

// ── The API over every fixture ──────────────────────────────────────────────

namespace {
std::vector<std::string> allFixtures() {
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(kFixtures)) {
        const auto ext = e.path().extension().string();
        if (ext == ".f32" || ext == ".py") continue;
        names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    REQUIRE(names.size() >= 30);
    return names;
}
} // namespace

TEST_CASE("audio fixtures: a probe agrees with a decode, and the file path with the bytes",
          "[audio][file][fixtures]") {
    for (const auto& name : allFixtures()) {
        INFO(name);
        const auto bytes = readFile(kFixtures / name);
        const auto probed = probeBytes(bytes);
        const auto fromBytes = decodeBytes(bytes);
        REQUIRE(fromBytes.status == CLOCKWORK_OK);
        CHECK(probed.format == fromBytes.info.format);
        CHECK(probed.encoding == fromBytes.info.encoding);
        CHECK(probed.channels == fromBytes.info.channels);
        CHECK(probed.sample_rate == fromBytes.info.sample_rate);
        CHECK(probed.source_bits == fromBytes.info.source_bits);
        // An MP3 with no Xing or LAME header says nothing about its length:
        // the probe says zero and a decode finds out.
        if (probed.frames != 0 || probed.format != CLOCKWORK_AUDIO_FORMAT_MP3)
            CHECK(probed.frames == fromBytes.info.frames);

        ClockworkAudioInfo info{};
        info.struct_bytes = sizeof(info);
        float* out = nullptr;
        REQUIRE(clockwork_audio_decode_file((kFixtures / name).string().c_str(), &info, &out)
                == CLOCKWORK_OK);
        REQUIRE(info.frames == fromBytes.info.frames);
        REQUIRE(info.channels == fromBytes.info.channels);
        CHECK(std::memcmp(out, fromBytes.samples.data(), fromBytes.samples.size() * sizeof(float)) == 0);
        clockwork_audio_free(out);

        ClockworkAudioInfo probedFile{};
        probedFile.struct_bytes = sizeof(probedFile);
        REQUIRE(clockwork_audio_probe_file((kFixtures / name).string().c_str(), &probedFile)
                == CLOCKWORK_OK);
        CHECK(probedFile.frames == probed.frames);
    }
}

TEST_CASE("audio fixtures: the same bytes decode the same on every thread at once",
          "[audio][file][fixtures]") {
    // Catch2's assertions are the test thread's alone, so the workers only
    // decode and keep what they got; every comparison is made back here.
    const std::vector<std::string> names = {"flac-s16.flac", "mp3-cbr.mp3", "ogg-q3.ogg", "wav-s24.wav"};
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<Decoded> alone;
    for (const auto& n : names) {
        bytes.push_back(readFile(kFixtures / n));
        alone.push_back(decodeBytes(bytes.back()));
        REQUIRE(alone.back().status == CLOCKWORK_OK);
    }

    struct Outcome { ClockworkStatus status; std::vector<float> samples; };
    const auto decodeQuietly = [](const std::vector<uint8_t>& in) {
        Outcome o{CLOCKWORK_E_ARG, {}};
        ClockworkAudioInfo info{};
        info.struct_bytes = sizeof(info);
        float* out = nullptr;
        o.status = clockwork_audio_decode_memory(in.data(), in.size(), &info, &out);
        if (o.status == CLOCKWORK_OK && out != nullptr) {
            o.samples.assign(out, out + static_cast<size_t>(info.frames) * info.channels);
            clockwork_audio_free(out);
        }
        return o;
    };

    constexpr int kThreads = 8, kRounds = 5;
    std::vector<std::vector<Outcome>> outcomes(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int round = 0; round < kRounds; ++round)
                for (const auto& b : bytes) outcomes[static_cast<size_t>(t)].push_back(decodeQuietly(b));
        });
    for (auto& t : threads) t.join();

    for (int t = 0; t < kThreads; ++t) {
        REQUIRE(outcomes[static_cast<size_t>(t)].size() == names.size() * kRounds);
        for (size_t i = 0; i < outcomes[static_cast<size_t>(t)].size(); ++i) {
            const auto& got = outcomes[static_cast<size_t>(t)][i];
            const auto& want = alone[i % names.size()];
            INFO("thread " << t << ", " << names[i % names.size()]);
            REQUIRE(got.status == CLOCKWORK_OK);
            REQUIRE(got.samples == want.samples);
        }
    }
}

// ── Files cut short, and bytes that are not audio ───────────────────────────

TEST_CASE("audio fixtures: a file cut short is refused or decoded short, never anything else",
          "[audio][file][fixtures]") {
    // Every fixture at four cuts: inside the header, just after it, half way
    // and near the end. The outcome is a status; a decode that says OK hands
    // back exactly the frames it claims; nothing crashes and nothing hangs.
    for (const auto& name : allFixtures()) {
        const auto whole = readFile(kFixtures / name);
        const auto full = decodeBytes(whole);
        REQUIRE(full.status == CLOCKWORK_OK);
        for (const size_t cut : {size_t(12), size_t(60), whole.size() / 2, whole.size() - 7}) {
            if (cut >= whole.size()) continue;
            INFO(name << " cut at " << cut << " of " << whole.size());
            const std::vector<uint8_t> part(whole.begin(), whole.begin() + static_cast<long>(cut));
            const auto d = decodeBytes(part);
            if (d.status == CLOCKWORK_OK) {
                CHECK(d.info.channels == full.info.channels);
                CHECK(d.info.frames <= full.info.frames);
                CHECK(d.samples.size() == static_cast<size_t>(d.info.frames) * d.info.channels);
            } else {
                CHECK((d.status == CLOCKWORK_E_ARG || d.status == CLOCKWORK_E_VERSION));
            }
        }
    }
}

TEST_CASE("audio fixtures: bytes that are not audio are refused", "[audio][file][fixtures]") {
    std::vector<uint8_t> noise(4096);
    uint32_t x = 0x9E3779B9u;
    for (auto& b : noise) { x = x * 1664525u + 1013904223u; b = static_cast<uint8_t>(x >> 24); }
    CHECK(decodeBytes(noise).status == CLOCKWORK_E_VERSION);
    CHECK(decodeBytes(std::vector<uint8_t>(64, 0)).status == CLOCKWORK_E_VERSION);
    CHECK(decodeBytes(std::vector<uint8_t>{'R', 'I', 'F', 'F'}).status == CLOCKWORK_E_VERSION);

    // A WAV header with no data chunk at all, and one with a data chunk of
    // nothing: zero frames is an answer, a crash is not.
    const auto headerOnly = craftWav(0, 0);
    const auto d = decodeBytes(headerOnly);
    if (d.status == CLOCKWORK_OK) CHECK(d.info.frames == 0);
    else CHECK(d.status == CLOCKWORK_E_ARG);
}

// ── What a header may claim, and what it gets ───────────────────────────────

TEST_CASE("audio fixtures: a header's claim does not size the allocation",
          "[audio][file][fixtures][security]") {
    // The data chunk says 100 kB of frames; the file holds 4410. What comes
    // back is what was there. The old decoders either refused the file or
    // handed back the claim padded with silence, and sized the buffer from
    // the claim either way.
    const auto generous = craftWav(100000 * 4, 4410);
    const auto d = decodeBytes(generous);
    REQUIRE(d.status == CLOCKWORK_OK);
    CHECK(d.info.frames == 4410);
    CHECK(d.samples.size() == 4410 * 2);
    CHECK(d.samples[0] == expected(0, 0, false));
    CHECK(d.samples.back() == expected(4409, 1, false));

    // The probe reads headers alone, but an uncompressed file's length is
    // bounded by its size, and that bound is what a GUI gets to size a buffer.
    CHECK(probeBytes(generous).frames == 4410);

    // The same with a claim of 0xFFFFFFF0 bytes, which is a billion frames and
    // eight gigabytes of float: the file holds a hundred frames, and that is
    // what comes back, with nothing sized from the claim.
    const auto wild = craftWav(0xFFFFFFF0u, 100);
    const auto w = decodeBytes(wild);
    REQUIRE(w.status == CLOCKWORK_OK);
    CHECK(w.info.frames == 100);
    CHECK(probeBytes(wild).frames == 100);
}

TEST_CASE("audio fixtures: a compressed claim past the decode limit is refused before anything is allocated",
          "[audio][file][fixtures][security]") {
    // A compressed file's length cannot be checked against its size, so the
    // claim is held to the limit instead: past it, the file is refused as
    // too big rather than attempted. The fuzzer found the FLAC form of this
    // (a 379-byte file asking for 496 GB) on 2026-10-27.

    // FLAC: the stream info's 36-bit total-samples field, set to its maximum.
    auto flac = readFile(kFixtures / "flac-s16.flac");
    REQUIRE(std::memcmp(flac.data(), "fLaC", 4) == 0);
    flac[21] |= 0x0F;                                   // top four bits of the count
    flac[22] = flac[23] = flac[24] = flac[25] = 0xFF;   // the rest
    CHECK(decodeBytes(flac).status == CLOCKWORK_E_TOO_BIG);

    // Ogg: the last page's granule position, which is the stream's length.
    // A page carries a checksum of itself, so the claim is re-sealed as an
    // attacker would seal it; a page with a bad checksum is a corrupt page,
    // not a claim.
    auto ogg = readFile(kFixtures / "ogg-q3.ogg");
    size_t lastPage = 0;
    for (size_t i = 0; i + 4 <= ogg.size(); ++i)
        if (std::memcmp(ogg.data() + i, "OggS", 4) == 0) lastPage = i;
    REQUIRE(lastPage > 0);
    for (int i = 0; i < 8; ++i) ogg[lastPage + 6 + static_cast<size_t>(i)] = i == 7 ? 0x7F : 0xFF;
    {
        const size_t segments = ogg[lastPage + 26];
        size_t pageLen = 27 + segments;
        for (size_t s = 0; s < segments; ++s) pageLen += ogg[lastPage + 27 + s];
        REQUIRE(lastPage + pageLen <= ogg.size());
        for (int i = 0; i < 4; ++i) ogg[lastPage + 22 + static_cast<size_t>(i)] = 0;
        uint32_t crc = 0;   // Ogg's CRC-32: polynomial 0x04c11db7, no reflection, no final xor
        for (size_t i = 0; i < pageLen; ++i) {
            crc ^= static_cast<uint32_t>(ogg[lastPage + i]) << 24;
            for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
        }
        for (int i = 0; i < 4; ++i) ogg[lastPage + 22 + static_cast<size_t>(i)] = static_cast<uint8_t>(crc >> (8 * i));
    }
    CHECK(decodeBytes(ogg).status == CLOCKWORK_E_TOO_BIG);
}

TEST_CASE("audio fixtures: a false claim under the limit decodes to what the file holds",
          "[audio][file][fixtures][security]") {
    // The same FLAC, claiming twice its length: a decoder that trusted the
    // claim would hand back half silence.
    auto flac = readFile(kFixtures / "flac-s16.flac");
    const uint32_t doubled = 4410 * 2;
    flac[21] = static_cast<uint8_t>(flac[21] & 0xF0);
    for (int i = 0; i < 4; ++i) flac[22 + i] = static_cast<uint8_t>(doubled >> (24 - 8 * i));
    CHECK(probeBytes(flac).frames == doubled);
    const auto d = decodeBytes(flac);
    REQUIRE(d.status == CLOCKWORK_OK);
    CHECK(d.info.frames == 4410);
    CHECK(d.samples.size() == 4410 * 2);
}

TEST_CASE("audio fixtures: the decode limit is the host's to set", "[audio][file][fixtures][security]") {
    // A small limit refuses a file that would decode past it, mid-decode if
    // need be: a lossless file's size is known from its header, a lossy
    // file's only as it is decoded, and both are held to it.
    const uint64_t before = clockwork_audio_decode_limit();
    CHECK(before >= 64u * 1024 * 1024);

    clockwork_audio_set_decode_limit(10000);
    CHECK(clockwork_audio_decode_limit() == 10000);
    CHECK(decodeFixture("wav-s16.wav").status == CLOCKWORK_E_TOO_BIG);   // 35 kB of float
    CHECK(decodeFixture("mp3-cbr.mp3").status == CLOCKWORK_E_TOO_BIG);   // the same, found while decoding
    CHECK(decodeFixture("ogg-q3.ogg").status == CLOCKWORK_E_TOO_BIG);
    CHECK(decodeFixture("wav-alaw.wav").status == CLOCKWORK_OK);         // 8 kB of float fits

    // A file larger than the limit is refused by the path forms before it is
    // read, whatever it holds: the limit bounds what a file costs.
    const auto big = kFixtures / "wav-f64.wav";                           // 70 kB on disk
    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* out = nullptr;
    CHECK(clockwork_audio_probe_file(big.string().c_str(), &info) == CLOCKWORK_E_TOO_BIG);
    CHECK(clockwork_audio_decode_file(big.string().c_str(), &info, &out) == CLOCKWORK_E_TOO_BIG);
    CHECK(out == nullptr);

    clockwork_audio_set_decode_limit(0);                                  // back to the default
    CHECK(clockwork_audio_decode_limit() == before);
    CHECK(decodeFixture("wav-s16.wav").status == CLOCKWORK_OK);
    CHECK(clockwork_audio_probe_file(big.string().c_str(), &info) == CLOCKWORK_OK);
}
