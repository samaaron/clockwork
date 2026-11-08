// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_link_audio_stand_in.cpp — a Link peer's audio, heard the same way on
 * every run.
 *
 * Everything a Link peer's audio meets in the engine, with none of it timed
 * by the machine. The peer is a stand-in (LinkAudioHost::addStandInChannel):
 * its channel is listed and subscribed over OSC as a Link peer's is, and its
 * audio is cut into Link's wire chunks and stamped on the session by Link's
 * own code, then handed to the subscription as Link's network would hand it.
 * Link stays off, so the session is this engine's own and no other Link app
 * on the machine can join it.
 *
 * The engine is pumped a device buffer at a time on a freewheel clock, and
 * the endpoint runs when the case says (LinkAudioHost::runEndpoint): between
 * device buffers, as a worker that is never late runs. So on every run the
 * engine counts the same underruns, resyncs and warps, and a case hears the
 * same silences in the same places; a late endpoint or a peer that stops
 * sending is something a case does on purpose. The samples themselves can
 * differ from run to run by a hundredth of a frame or so: the session's
 * timeline begins when Link is made, on the machine's clock, and where its
 * microseconds fall against the listening moves the renderer's rounding.
 *
 * Discovery, the wire and a peer in another process are left to SuperSonic's
 * test_link_audio_integration.cpp, which needs real Link.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/LinkAudioBridge.h"   // decides CLOCKWORK_LINK_AUDIO

#if CLOCKWORK_LINK_AUDIO

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"   // clockwork_audio_in

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using link_audio::ConnectionState;

constexpr uint32_t kBlock   = 64;      // the engine's DSP block
constexpr double   kTau     = 6.283185307179586;
constexpr double   kHz      = 440.0;   // the peer's sine
constexpr double   kLevel   = 0.5;
// Heard from this long after the audio first arrives: the converter's and
// the renderer's start.
constexpr double   kSettleSeconds = 0.25;
// A sample within this much of a frame of where the peer's stamps put it is
// the peer's audio. The stamps and the read dates are whole microseconds and
// the renderer steers out what that leaves, so a stream heard whole wanders
// by a few hundredths of a frame (under 0.15 at either rate).
constexpr double   kOnSineFrames = 0.25;
// After a hole in the peer's stream, the stream comes back a fraction of a
// millisecond from where its stamps put it and is steered onto it within
// this (31 frames out, 70 ms to steer, at 48 kHz).
constexpr double   kRejoinSeconds = 0.1;
// A run of exact zeros this long is silence a short read left: a sine is
// exactly 0 for a sample at most.
constexpr size_t   kSilentRun = 8;
// Either side of a silence the converter is filling in from nothing, or
// fading out to it: its filter's reach at these rates.
constexpr size_t   kEdgeFrames = 64;

// Listening starts at this block, whatever the setup took. An OSC exchange
// pumps blocks until the control thread answers, as many as it takes, and the
// block clock's microseconds fall differently from one block to the next
// (64 frames at 48 kHz is 1333.3 us), so a stream started on another block is
// read on dates a microsecond apart: heard a little differently, and on
// another block when it rejoins after a hole.
constexpr uint64_t kListenFromBlock = 1024;

// The engine rates and device buffers a case is heard at.
struct Device { int rate; uint32_t frames; };
constexpr Device kDevices[] = { {48000, 64}, {44100, 512}, {48000, 512} };

ClockworkEngine::Config exactConfig(int rate) {
    auto cfg = EngineFixture::defaultConfig();
    cfg.sampleRate      = rate;
    cfg.bufferSize      = static_cast<int>(kBlock);
    cfg.blockSize       = static_cast<int>(kBlock);
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}

// The input channel the engine put the subscription on, or -1 if refused.
int32_t subscribe(EngineFixture& fx, const char* peer, const char* channel) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("clock/audio/input/add")) << peer << channel;
    fx.clearReplies();
    fx.send(b.end());
    OscReply r;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/audio/input/add.reply"), r));
    const auto p = r.parsed();
    return p.argInt(0) == 1 ? p.argInt(1) : -1;
}

// An engine, a stand-in peer sending a sine, and the subscription between
// them, heard as a device of `deviceFrames` drives the engine: a callback's
// blocks at once, the peer sending what it has made by the time each
// callback begins, and the endpoint run before it unless the case has made
// it late. What the subscription's left channel carried is kept.
struct Rig {
    EngineFixture  fx;
    LinkAudioHost& link;
    const int      rate;
    const uint32_t deviceFrames;
    int32_t        channel = -1;
    std::vector<float> heard;
    bool           endpointLate = false;

    // The peer: 48 kHz stereo, `peerBlock` frames a buffer, stamped on
    // Link's clock from its own frame count, from `t0`: when the first block
    // heard is read. It plays `signal`, the sine unless a case says otherwise.
    const uint32_t peerRate = 48000;
    const uint32_t peerBlock;
    uint64_t       t0 = 0;
    uint64_t       frame = 0;
    std::vector<float> samples;
    std::function<double(uint64_t)> signal = [this](uint64_t f) { return sineAt(f); };

    // `setup` is the case's own, done over OSC before the listening starts.
    Rig(int engineRate, uint32_t device, uint32_t peerBuffer = 64,
        const std::function<void(EngineFixture&)>& setup = {})
        : fx(exactConfig(engineRate)), link(fx.engine().linkAudio()), rate(engineRate),
          deviceFrames(device), peerBlock(peerBuffer) {
        link.setEndpointByHand(true);
        link.addStandInChannel("FakeLive", "Main");
        channel = subscribe(fx, "FakeLive", "Main");
        REQUIRE(channel >= 0);
        if (setup) setup(fx);
        INFO("the setup took " << link.datedBlocks() << " blocks");
        REQUIRE(link.datedBlocks() <= kListenFromBlock);
        while (link.datedBlocks() < kListenFromBlock) fx.pumpBlock();
    }

    double sineAt(uint64_t f) const { return kLevel * std::sin(kTau * kHz * double(f) / double(peerRate)); }
    uint64_t microsOf(uint64_t f) const {
        return t0 + static_cast<uint64_t>(std::llround(1e6 * double(f) / double(peerRate)));
    }

    void peerSends() {
        const uint64_t now = link.nextReadMicros();
        samples.resize(size_t(peerBlock) * 2);
        while (microsOf(frame) <= now) {
            for (uint32_t i = 0; i < peerBlock; ++i)
                samples[2 * i] = samples[2 * i + 1] = static_cast<float>(signal(frame + i));
            REQUIRE(link.sendStandInAudio("FakeLive", "Main", samples.data(), peerBlock, 2,
                                          peerRate, microsOf(frame)));
            frame += peerBlock;
        }
    }

    // The peer's callback stops for `seconds`: what it would have made in
    // that time is never made, and it goes on stamped at the present.
    void peerStalls(double seconds) {
        frame += static_cast<uint64_t>(seconds * peerRate) / peerBlock * peerBlock;
    }

    void callback() {
        // The peer starts with the listening, not before: an OSC exchange
        // pumps blocks until its reply comes, as many as the control thread
        // takes, and the stream must not start a block earlier or later.
        if (heard.empty()) t0 = link.nextReadMicros();
        peerSends();
        if (!endpointLate) link.runEndpoint();
        ClockworkCore* core = fx.engine().core();
        fx.engine().pumpAudioCallback(deviceFrames, [&] {
            const float* in = clockwork_audio_in(core) + size_t(channel) * kBlock;
            heard.insert(heard.end(), in, in + kBlock);
        });
    }

    void listen(double seconds) {
        const size_t want = heard.size() + static_cast<size_t>(seconds * rate);
        while (heard.size() < want) callback();
    }

    // Where the peer's frame `f` is due in what was heard: its stamp, plus
    // the input's latency.
    int64_t dueAt(uint64_t f, double latencySeconds) const {
        const double micros = double(microsOf(f) - t0) + 1e6 * latencySeconds;
        return static_cast<int64_t>(std::llround(micros * double(rate) / 1e6));
    }

    size_t firstAudio() const {
        const auto it = std::find_if(heard.begin(), heard.end(), [](float s) { return s != 0.0f; });
        return static_cast<size_t>(it - heard.begin());
    }

    link_audio::InputStatus input() const {
        const auto inputs = link.listInputs();
        REQUIRE(inputs.size() == 1);
        return inputs[0];
    }
};

// What was heard from `from` on, against the peer's sine at the engine's
// rate: its phase and level fitted by least squares over the first 100 ms.
// A run of at least kSilentRun exact zeros is silence. Any other sample is on
// the sine when it is within kOnSineFrames of where the sine puts it, at the
// edge of a silence within kEdgeFrames of one, rejoining within `rejoin`
// frames after a silence ends, and off otherwise.
struct Judged {
    size_t silences = 0;       // runs of silence
    size_t silentFrames = 0;
    size_t rejoining = 0;      // off the sine just after a silence
    size_t off = 0;            // off the sine anywhere else
    size_t firstOff = SIZE_MAX;
    double worstFrames = 0.0;  // the farthest a sample on the sine is from it, in frames
};

Judged judge(const std::vector<float>& s, size_t from, int rate, size_t rejoin = 0) {
    Judged j;
    const double w = kTau * kHz / double(rate);
    // Most a sample moves for a timing error of a frame: the sine's slope.
    const double perFrame = kLevel * w;
    const size_t fit = static_cast<size_t>(0.1 * rate);
    REQUIRE(from + fit <= s.size());
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;
    for (size_t n = from; n < from + fit; ++n) {
        const double sn = std::sin(w * double(n)), cn = std::cos(w * double(n));
        ss += sn * sn; sc += sn * cn; cc += cn * cn;
        xs += s[n] * sn; xc += s[n] * cn;
    }
    const double det = ss * cc - sc * sc;
    const double a = (xs * cc - xc * sc) / det;
    const double b = (xc * ss - xs * sc) / det;

    std::vector<bool> silent(s.size(), false);
    std::vector<size_t> ends;
    for (size_t n = from; n < s.size();) {
        size_t m = n;
        while (m < s.size() && s[m] == 0.0f) ++m;
        if (m - n >= kSilentRun) {
            ++j.silences;
            j.silentFrames += m - n;
            ends.push_back(m);
            std::fill(silent.begin() + long(n), silent.begin() + long(m), true);
        }
        n = m == n ? n + 1 : m;
    }
    const auto rejoining = [&](size_t n) {
        return std::any_of(ends.begin(), ends.end(), [&](size_t e) { return n >= e && n < e + rejoin; });
    };
    const auto nearSilence = [&](size_t n) {
        const size_t lo = n > kEdgeFrames ? n - kEdgeFrames : 0;
        const size_t hi = std::min(s.size(), n + kEdgeFrames + 1);
        for (size_t k = lo; k < hi; ++k) if (silent[k]) return true;
        return false;
    };
    for (size_t n = from; n < s.size(); ++n) {
        if (silent[n]) continue;
        const double d = std::fabs(double(s[n]) - (a * std::sin(w * double(n)) + b * std::cos(w * double(n))));
        if (d <= kOnSineFrames * perFrame) { j.worstFrames = std::max(j.worstFrames, d / perFrame); continue; }
        if (nearSilence(n)) continue;
        if (rejoining(n)) { ++j.rejoining; continue; }
        if (j.off++ == 0) j.firstOff = n;
    }
    return j;
}

std::string account(const Rig& rig, const Judged& j, const link_audio::InputStatus& in) {
    std::ostringstream o;
    o << "engine " << rig.rate << " Hz, device buffer " << rig.deviceFrames << ", peer buffer "
      << rig.peerBlock << ": heard " << rig.heard.size() << " frames, audio from frame "
      << rig.firstAudio() << "; " << j.silences << " silences (" << j.silentFrames << " frames), "
      << j.rejoining << " samples rejoining, " << j.off << " samples off the sine (first at "
      << j.firstOff << "), the rest within " << j.worstFrames << " frames"
      << "; the engine counted " << in.underruns << " underruns, " << in.resyncs << " resyncs, "
      << in.warps << " warps, drift " << in.driftPpm << " ppm, state " << int(in.state);
    return o.str();
}

} // namespace

TEST_CASE("Link Audio: a stand-in peer's channel is listed and subscribed over OSC as a Link peer's is",
          "[link-audio][stand-in]") {
    EngineFixture fx{exactConfig(48000)};
    auto& link = fx.engine().linkAudio();
    link.addStandInChannel("FakeLive", "Main");

    fx.clearReplies();
    fx.send(osc_test::message(CLOCKWORK_SYS("clock/audio/channels/get")));
    OscReply listed;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/audio/channels.reply"), listed));
    const auto p = listed.parsed();
    REQUIRE(p.argInt(0) == 1);
    CHECK(p.argString(2) == "Main");       // [channelId channelName peerId peerName]
    CHECK(p.argString(4) == "FakeLive");

    CHECK(subscribe(fx, "FakeLive", "Main") >= 0);
    const auto inputs = link.listInputs();
    REQUIRE(inputs.size() == 1);
    CHECK(inputs[0].state == ConnectionState::Connecting);
    CHECK(subscribe(fx, "FakeLive", "Nothing") == -1);
}

TEST_CASE("Link Audio: a peer's sine is heard whole at every engine rate, device buffer and peer buffer",
          "[link-audio][stand-in]") {
    for (const uint32_t peerBlock : {64u, 1024u}) {
        for (const auto& [rate, frames] : kDevices) {
            DYNAMIC_SECTION("engine " << rate << " Hz, device buffer " << frames << ", peer buffer " << peerBlock) {
                Rig rig(rate, frames, peerBlock);
                rig.listen(2.0);
                const size_t from = rig.firstAudio() + static_cast<size_t>(kSettleSeconds * rate);
                const Judged j = judge(rig.heard, from, rate);
                const auto in = rig.input();
                INFO(account(rig, j, in));
                CHECK(j.silences == 0);
                CHECK(j.off == 0);
                CHECK(in.underruns == 0);
                CHECK(in.resyncs == 0);
                CHECK(in.warps == 0);
                CHECK(in.state == ConnectionState::Connected);
            }
        }
    }
}

TEST_CASE("Link Audio: a peer that stops sending for 100 ms is heard as silence counted as underruns, "
          "shows Dropout, carries on at pitch, and is Connected again a second later",
          "[link-audio][stand-in]") {
    for (const auto& [rate, frames] : kDevices) {
        DYNAMIC_SECTION("engine " << rate << " Hz, device buffer " << frames) {
            Rig rig(rate, frames);
            rig.listen(1.0);
            rig.peerStalls(0.1);
            rig.listen(0.5);
            const auto during = rig.input();
            rig.listen(1.2);
            const size_t from = rig.firstAudio() + static_cast<size_t>(kSettleSeconds * rate);
            const Judged j = judge(rig.heard, from, rate, static_cast<size_t>(kRejoinSeconds * rate));
            const auto in = rig.input();
            INFO(account(rig, j, in));
            CHECK(j.silences == 1);
            // The hole, give or take the device buffer it began and ended in,
            // and the converter's reach before it speaks again.
            CHECK(j.silentFrames + frames >= static_cast<size_t>(0.1 * rate));
            CHECK(j.silentFrames <= static_cast<size_t>(0.1 * rate) + frames + kEdgeFrames);
            CHECK(j.off == 0);
            CHECK(in.underruns == j.silentFrames);
            CHECK(in.resyncs == 1);
            CHECK(in.warps == 0);
            CHECK(during.state == ConnectionState::Dropout);
            CHECK(in.state == ConnectionState::Connected);
        }
    }
}

TEST_CASE("Link Audio: an endpoint 200 ms late is heard as silence counted as underruns, "
          "and the stream goes on from the present",
          "[link-audio][stand-in]") {
    for (const auto& [rate, frames] : kDevices) {
        DYNAMIC_SECTION("engine " << rate << " Hz, device buffer " << frames) {
            Rig rig(rate, frames);
            rig.listen(1.0);
            rig.endpointLate = true;
            rig.listen(0.2);
            rig.endpointLate = false;
            rig.listen(1.0);
            const size_t from = rig.firstAudio() + static_cast<size_t>(kSettleSeconds * rate);
            const Judged j = judge(rig.heard, from, rate);
            const auto in = rig.input();
            INFO(account(rig, j, in));
            CHECK(j.silences == 1);
            CHECK(j.off == 0);
            CHECK(in.underruns == j.silentFrames);
            CHECK(in.resyncs == 1);
            CHECK(in.warps == 0);
        }
    }
}

TEST_CASE("Link Audio: the OSC input report carries the counts the engine keeps",
          "[link-audio][stand-in]") {
    Rig rig(48000, 64);
    rig.listen(1.0);
    rig.peerStalls(0.1);
    rig.listen(0.5);
    rig.link.runEndpoint();   // a full port: the blocks the reply waits for read audio

    rig.fx.clearReplies();
    rig.fx.send(osc_test::message(CLOCKWORK_SYS("clock/audio/inputs/get")));
    OscReply r;
    REQUIRE(rig.fx.waitForReply(CLOCKWORK_SYS("clock/audio/inputs.reply"), r));
    const auto in = rig.input();
    const auto p = r.parsed();
    // <count> then, for each input: peer channel inputChannel rate sourceChannels
    // bufferedMs state dropped networkGaps sourceCalls duplicates latency
    // underruns resyncs warps driftPpm.
    REQUIRE(p.argInt(0) == 1);
    CHECK(p.argString(1) == "FakeLive");
    CHECK(p.argString(2) == "Main");
    CHECK(p.argInt(3) == rig.channel);
    CHECK(p.argInt(7) == int(in.state));
    CHECK(uint64_t(p.argInt(13)) == in.underruns);
    CHECK(uint64_t(p.argInt(14)) == in.resyncs);
    CHECK(uint64_t(p.argInt(15)) == in.warps);
    CHECK(in.underruns > 0);
    CHECK(in.resyncs == 1);
}

TEST_CASE("Link Audio: an input's latency setting delays what is heard by exactly that much",
          "[link-audio][stand-in]") {
    // A click at the peer's frame 4800, 100 ms into its stream, heard where
    // its stamp and the latency put it, to the frame.
    constexpr uint64_t kClickFrame = 4800;
    for (const double latency : {0.05, 0.15}) {
        DYNAMIC_SECTION("latency " << latency << " s") {
            Rig rig(48000, 64, 64, [&](EngineFixture& fx) {
                osc_test::Builder b;
                b.begin(CLOCKWORK_SYS("clock/audio/input/latency/set")) << "FakeLive" << "Main"
                                                                        << static_cast<float>(latency);
                fx.clearReplies();
                fx.send(b.end());
                OscReply r;
                REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/audio/input/latency/set.reply"), r));
                REQUIRE(r.parsed().argInt(0) == 1);
            });
            rig.signal = [](uint64_t f) { return f == kClickFrame ? kLevel : 0.0; };
            rig.listen(0.5);
            const auto peak = std::max_element(rig.heard.begin(), rig.heard.end(),
                [](float a, float b) { return std::fabs(a) < std::fabs(b); });
            const auto heardAt = static_cast<int64_t>(peak - rig.heard.begin());
            const auto due = rig.dueAt(kClickFrame, latency);
            INFO("the click heard at frame " << heardAt << ", " << *peak << ", due at " << due);
            CHECK(std::llabs(heardAt - due) <= 1);
            CHECK(std::fabs(*peak) > 0.25f);
        }
    }
}

#endif  // CLOCKWORK_LINK_AUDIO
