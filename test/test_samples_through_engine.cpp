// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_samples_through_engine.cpp — real sample files, the whole way round.
 *
 * Every audio file in a directory goes through everything a product does
 * with one, through the same calls a product makes, with ffmpeg as the
 * judge at both ends:
 *
 *   1. decoded by clockwork_audio_decode_file, and compared with ffmpeg's
 *      decode of the same file, sample for sample;
 *   2. played through the engine: produced into a source port on the DSP's
 *      input, copied to the output by the placeholder's echo, taken from a
 *      sink port, and found to be the same frames, exactly;
 *   3. written back out by clockwork_audio_writer as FLAC at 16 and 24 bits
 *      and as float WAV, each decoded by ffmpeg and by clockwork, both of
 *      which must give the frames that went in, to the depth's quantisation.
 *
 * The engine is the lanes fixture: booted once, pumped a block at a time on
 * this thread, no clock and no device, so eighteen million frames of real
 * samples take seconds, deterministically.
 *
 * By default the directory is test/fixtures/audio. CLOCKWORK_SAMPLES_DIR
 * points it at any other, and scripts/check-samples.sh points it at every
 * sample Sonic Pi ships, locally and in CI. Needs ffmpeg and ffprobe.
 */
#include <catch2/catch_test_macros.hpp>
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "clockwork_audio_file.h"
#include "clockwork_path.h"
#include "clockwork_port_bus.h"
#include "clockwork_ports.h"
#include "lanes/lanes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>   // getpid
#else
#include <process.h>
#define getpid _getpid
#endif

namespace {

namespace fs = std::filesystem;

// Of full scale, for MP3 and Ogg against ffmpeg: arithmetic precision, as
// test_audio_fixtures.cpp says (measured 1.4e-7, this is a hundred times that).
constexpr float kLossyTolerance = 1e-5f;

struct Ports {
    ~Ports() { clockwork_port_bus_detach_all(); clockwork_port_close_all(); }
};

struct Decoded {
    ClockworkAudioInfo info{};
    std::vector<float> samples;
};

bool isAudio(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const char* known : {".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".mp3", ".ogg", ".oga", ".w64", ".rf64"})
        if (ext == known) return true;
    return false;
}

std::vector<fs::path> audioFilesUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file() && isAudio(e.path())) out.push_back(e.path());
    std::sort(out.begin(), out.end());
    return out;
}

// The directory under test: the fixtures, unless the environment says otherwise.
fs::path samplesDir() {
    if (const char* d = std::getenv("CLOCKWORK_SAMPLES_DIR"); d && *d) return fs::path(d);
    return fs::path(CLOCKWORK_AUDIO_FIXTURES);
}

std::string shellQuote(const fs::path& p) {
    std::string s = "'";
    for (const char c : clockwork_path::to_utf8(p)) {
        if (c == '\'') s += "'\\''";
        else s += c;
    }
    return s + "'";
}

// What a command prints, as bytes.
std::vector<uint8_t> run(const std::string& command) {
    std::vector<uint8_t> out;
    std::FILE* pipe = ::popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, pipe)) > 0;) out.insert(out.end(), buf, buf + n);
    REQUIRE(::pclose(pipe) == 0);
    return out;
}

// ffmpeg's decode of a file: interleaved 32-bit float. Its Vorbis decode runs
// on past the stream's end by the last window's overlap, where every other
// decoder stops at the granule position, so an Ogg is cut to the length
// ffprobe reports for it.
std::vector<float> ffmpegDecode(const fs::path& file) {
    std::string trim;
    std::string ext = file.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == ".ogg" || ext == ".oga") {
        const auto probed = run("ffprobe -v error -show_entries stream=duration_ts -of csv=p=0 " + shellQuote(file));
        const std::string frames(probed.begin(), probed.end());
        trim = " -af atrim=end_sample=" + std::to_string(std::strtoull(frames.c_str(), nullptr, 10));
    }
    const auto raw = run("ffmpeg -v error -i " + shellQuote(file) + trim + " -f f32le -c:a pcm_f32le -");
    std::vector<float> f(raw.size() / sizeof(float));
    std::memcpy(f.data(), raw.data(), f.size() * sizeof(float));
    return f;
}

Decoded decodeWithClockwork(const fs::path& file) {
    Decoded d;
    d.info.struct_bytes = sizeof(d.info);
    float* out = nullptr;
    const auto st = clockwork_audio_decode_file(clockwork_path::to_utf8(file).c_str(), &d.info, &out);
    INFO(file.filename().string() << ": decode status " << st);
    REQUIRE(st == CLOCKWORK_OK);
    d.samples.assign(out, out + static_cast<size_t>(d.info.frames) * d.info.channels);
    clockwork_audio_free(out);
    return d;
}

// The largest difference between two decodes, or -1 when their lengths differ.
float worstDifference(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return -1.0f;
    float worst = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
    return worst;
}

// Through the engine: produced into a source port the DSP reads as input,
// echoed to the output, taken from a sink port. Returns what came out.
std::vector<float> throughTheEngine(const std::vector<float>& in, uint32_t channels) {
    const uint32_t bl = lanes_test::boot();
    const auto echo = osc_test::message("/dummy/echo");
    REQUIRE(lanes_test::ingress(echo.ptr(), echo.size(), 0));
    lanes_test::tick();
    lanes_test::drainRt();

    const uint32_t capacity = 8192;
    const ClockworkPort src  = clockwork_port_open("samples-in",  kClockworkPortSource, channels, capacity);
    const ClockworkPort sink = clockwork_port_open("samples-out", kClockworkPortSink,   channels, capacity);
    REQUIRE(src != CLOCKWORK_PORT_NONE);
    REQUIRE(sink != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(src, 0) == 1);
    REQUIRE(clockwork_port_bus_attach(sink, 0) == 1);

    const uint64_t frames = in.size() / channels;
    // Padded to whole blocks, so the source never runs dry: every block the
    // DSP reads was produced, and the tail is explicit silence.
    const uint64_t padded = (frames + bl - 1) / bl * bl;
    std::vector<float> out;
    out.reserve(static_cast<size_t>(padded) * channels);
    std::vector<float> zeros(static_cast<size_t>(bl) * channels, 0.0f);
    std::vector<float> block(static_cast<size_t>(bl) * channels);
    uint64_t produced = 0;
    while (out.size() < static_cast<size_t>(padded) * channels) {
        while (produced < padded && clockwork_port_writable(src) >= bl) {
            const uint64_t left = frames > produced ? frames - produced : 0;
            const uint32_t real = static_cast<uint32_t>(std::min<uint64_t>(bl, left));
            if (real == bl) {
                REQUIRE(clockwork_port_produce(src, in.data() + produced * channels, bl) == bl);
            } else {
                std::copy(zeros.begin(), zeros.end(), block.begin());
                if (real > 0) std::copy_n(in.data() + produced * channels, static_cast<size_t>(real) * channels, block.begin());
                REQUIRE(clockwork_port_produce(src, block.data(), bl) == bl);
            }
            produced += bl;
        }
        lanes_test::tick();
        while (clockwork_port_readable(sink) >= bl) {
            REQUIRE(clockwork_port_consume(sink, block.data(), bl) == bl);
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    CHECK(clockwork_port_underruns(src) == 0);
    CHECK(clockwork_port_overruns(sink) == 0);

    clockwork_port_bus_detach(src);
    clockwork_port_bus_detach(sink);
    clockwork_port_close(src);
    clockwork_port_close(sink);
    const auto restore = osc_test::message("/dummy/pulse", 10, 500);
    REQUIRE(lanes_test::ingress(restore.ptr(), restore.size(), 0));
    lanes_test::tick();
    lanes_test::drainRt();

    out.resize(static_cast<size_t>(frames) * channels);
    return out;
}

// Float to the integer a depth holds, as the writer does it.
int32_t quantize(float v, int32_t peak) {
    const float scaled = v * static_cast<float>(peak + 1);
    if (scaled >= static_cast<float>(peak)) return peak;
    if (scaled <= static_cast<float>(-peak - 1)) return -peak - 1;
    return static_cast<int32_t>(std::lrintf(scaled));
}

struct Written {
    const char* leaf;
    uint32_t format;
    uint32_t encoding;
    int32_t peak;   // 0: float, exact
};

// Written out by the C writer, read back by ffmpeg and by clockwork: both
// must give the frames that went in, at the depth's quantisation.
void writeAndReadBack(const fs::path& tmp, const std::string& name, const Decoded& d) {
    for (const Written w : {Written{"s16.flac", CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_S16, 32767},
                            Written{"s24.flac", CLOCKWORK_AUDIO_FORMAT_FLAC, CLOCKWORK_AUDIO_ENCODING_PCM_S24, 8388607},
                            Written{"f32.wav",  CLOCKWORK_AUDIO_FORMAT_WAV,  CLOCKWORK_AUDIO_ENCODING_FLOAT32, 0}}) {
        INFO(name << " written as " << w.leaf);
        const fs::path file = tmp / (name + "." + w.leaf);
        ClockworkAudioWriterConfig cfg{};
        cfg.struct_bytes = sizeof(cfg);
        cfg.format = w.format;
        cfg.encoding = w.encoding;
        cfg.channels = d.info.channels;
        cfg.sample_rate = d.info.sample_rate;
        ClockworkStatus st = CLOCKWORK_E_ARG;
        ClockworkAudioWriter* writer = clockwork_audio_writer_open(clockwork_path::to_utf8(file).c_str(), &cfg, &st);
        REQUIRE(writer != nullptr);
        // In ragged chunks, so a block boundary never lines up with a call.
        uint64_t at = 0, chunk = 1;
        while (at < d.info.frames) {
            const uint64_t n = std::min<uint64_t>(chunk, d.info.frames - at);
            REQUIRE(clockwork_audio_writer_write(writer, d.samples.data() + at * d.info.channels, n) == CLOCKWORK_OK);
            at += n;
            chunk = (chunk * 7 + 13) % 3000 + 1;
        }
        REQUIRE(clockwork_audio_writer_close(writer, nullptr, nullptr) == CLOCKWORK_OK);

        std::vector<float> expected(d.samples.size());
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = w.peak == 0 ? d.samples[i]
                                      : static_cast<float>(quantize(d.samples[i], w.peak)) / static_cast<float>(w.peak + 1);

        const auto byFfmpeg = ffmpegDecode(file);
        INFO("ffmpeg read " << byFfmpeg.size() << " samples, " << expected.size() << " went in");
        REQUIRE(worstDifference(byFfmpeg, expected) == 0.0f);

        const auto byClockwork = decodeWithClockwork(file);
        CHECK(byClockwork.info.channels == d.info.channels);
        CHECK(byClockwork.info.sample_rate == d.info.sample_rate);
        REQUIRE(worstDifference(byClockwork.samples, expected) == 0.0f);
        fs::remove(file);
    }
}

} // namespace

// Whether a judge is on the path at all. Without one the case skips over the
// fixtures, visibly; pointed at a directory, it fails instead: a run that
// asked for the samples and could not judge them did not look.
bool haveFfmpeg() {
    std::FILE* pipe = ::popen("ffmpeg -version >/dev/null 2>&1 && ffprobe -version >/dev/null 2>&1", "r");
    return pipe != nullptr && ::pclose(pipe) == 0;
}

TEST_CASE("samples: every file decodes as ffmpeg decodes it, plays through the engine unchanged, "
          "and writes back out to what ffmpeg and clockwork both read as the same frames",
          "[samples][audio][ports]") {
    if (!haveFfmpeg()) {
        REQUIRE_FALSE(std::getenv("CLOCKWORK_SAMPLES_DIR"));   // asked for, and no judge: a failure
        SKIP("ffmpeg and ffprobe are not on the path");
    }
    Ports guard;
    const fs::path dir = samplesDir();
    const auto files = audioFilesUnder(dir);
    INFO("directory " << dir.string());
    REQUIRE(!files.empty());

    // This process's own, so two runs at once (ctest runs cases in parallel
    // processes) never take each other's files.
    const fs::path tmp = fs::temp_directory_path() / ("clockwork_samples_test_" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    uint64_t framesInAll = 0;
    size_t throughEngine = 0;
    for (const auto& file : files) {
        const std::string name = file.filename().string();
        INFO(name);

        // 1. Decoded, against ffmpeg.
        const Decoded d = decodeWithClockwork(file);
        framesInAll += d.info.frames;
        const bool lossy = d.info.format == CLOCKWORK_AUDIO_FORMAT_MP3 || d.info.format == CLOCKWORK_AUDIO_FORMAT_OGG;
        const auto reference = ffmpegDecode(file);
        const float worst = worstDifference(d.samples, reference);
        INFO("clockwork " << d.samples.size() << " samples, ffmpeg " << reference.size() << ", largest difference " << worst);
        REQUIRE(worst >= 0.0f);
        REQUIRE(worst <= (lossy ? kLossyTolerance : 0.0f));

        // 2. Through the engine, on the channels the fixture has.
        if (d.info.channels <= lanes_test::kInChannels && d.info.frames > 0) {
            const auto played = throughTheEngine(d.samples, d.info.channels);
            REQUIRE(worstDifference(played, d.samples) == 0.0f);
            ++throughEngine;
        }

        // 3. Written back out, read by both.
        writeAndReadBack(tmp, name, d);
    }
    fs::remove_all(tmp);
    INFO(files.size() << " files, " << throughEngine << " through the engine, " << framesInAll << " frames in all");
    CHECK(throughEngine > 0);
}
