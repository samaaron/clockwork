// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_link_audio_input_renderer.cpp — a peer's stream, placed on the timeline.
 *
 * The renderer is handed a peer's buffers, each stamped with the beat its
 * first frame falls on, and asked for frames as of a host time: the audio the
 * peer was making then. Everything here is one Link, never enabled, so the
 * session is this process's own and every beat is exact; the peer is a sine
 * stamped from its sample clock, and the host is whatever the case says.
 *
 * Contract pinned (src/native/vendor/LinkAudioInputRenderer.hpp):
 *   - a stream is rendered from where its stamps put it, sample for sample;
 *   - consecutive renders are one continuous stream, no warp, no resync;
 *   - a few frames' disagreement between the clocks is steered over the
 *     render, not a lost place;
 *   - after the host has gone on without the stream — a stall, the ring read
 *     dry — the stream rejoins the present: the audio of now, not the audio
 *     that was missed played fast to catch up; one resync, no warp;
 *   - a hole in the peer's stream is nothing to render, then the stream as it
 *     resumes; one resync, no warp;
 *   - a clock re-measurement that moves the timeline under a stream by tens
 *     of milliseconds is steered to, not a lost place: no resync, no warp.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "native/LinkAudioBridge.h"   // decides CLOCKWORK_LINK_AUDIO

#if CLOCKWORK_LINK_AUDIO

#include "native/vendor/LinkAudioInputRenderer.hpp"

#include <ableton/LinkAudio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace {

using Renderer = clockwork_link::LinkAudioInputRenderer<ableton::LinkAudio>;
using Micros   = std::chrono::microseconds;

constexpr double   kQuantum   = 4.0;
constexpr double   kTau       = 6.283185307179586;
constexpr uint32_t kRate      = 48000;
constexpr size_t   kPeerBlock = 64;      // a Sonic Pi peer's
constexpr size_t   kRender    = 512;     // the endpoint's chunk
constexpr double   kHz        = 440.0;

// The session is this process's own; a peer stamps buffers on it and a host
// asks on it.
struct Session {
    ableton::LinkAudio link{120.0, "renderer test"};
    ableton::LinkAudio::SessionState state = link.captureAppSessionState();
    ableton::link::SessionId id = ableton::detail::linkApiState(state).timelineSessionId;
    double beatAt(Micros t) const { return state.beatAtTime(t, kQuantum); }
};

// A peer: a 440 Hz sine on the left, silence on the right, 64 frames a buffer,
// stamped from its sample clock from `t0`. Its frame counter is time: what it
// skips was never made, as a stalled callback never makes it.
struct Peer {
    Session&             session;
    Renderer&            renderer;
    Micros               t0;
    uint64_t             frame = 0;
    uint64_t             count = 1;
    std::vector<int16_t> samples = std::vector<int16_t>(kPeerBlock * 2);
    // Stamp jitter, in frames: a real publisher's stamps are rounded through
    // beats and microseconds and do not abut exactly. Deterministic.
    double               jitterFrames = 0.0;
    uint32_t             lcg = 12345;

    double               hz = kHz;   // the sine the peer plays

    double sine(uint64_t frame) const {
        return std::sin(kTau * hz * double(frame) / double(kRate));
    }
    Micros timeOf(uint64_t f) const {
        return t0 + Micros(static_cast<int64_t>(std::llround(1e6 * double(f) / double(kRate))));
    }
    void publish(size_t buffers) {
        for (size_t b = 0; b < buffers; ++b) {
            for (size_t i = 0; i < kPeerBlock; ++i) {
                samples[2 * i]     = ableton::util::floatToInt16(static_cast<float>(sine(frame + i)));
                samples[2 * i + 1] = 0;
            }
            ableton::LinkAudioSource::BufferHandle bh{};
            bh.samples              = samples.data();
            bh.info.numChannels     = 2;
            bh.info.numFrames       = kPeerBlock;
            bh.info.sampleRate      = kRate;
            bh.info.count           = count++;
            double stampFrame = double(frame);
            if (jitterFrames > 0.0) {
                lcg = lcg * 1664525u + 1013904223u;
                stampFrame += jitterFrames * (2.0 * double(lcg >> 8) / double(1u << 24) - 1.0);
            }
            bh.info.sessionBeatTime = session.beatAt(
                t0 + Micros(static_cast<int64_t>(std::llround(1e6 * stampFrame / double(kRate)))));
            bh.info.tempo           = session.state.tempo();
            bh.info.sessionId       = session.id;
            renderer.push(bh);
            frame += kPeerBlock;
        }
    }
    // Time passes for the peer with nothing made: a stalled callback.
    void skip(size_t frames) { frame += frames; }
};

// What one render gave, and how it compared with the peer's own sine as of
// the host time asked for.
struct Rendered {
    size_t frames   = 0;
    double maxError = 0.0;   // against the peer's sine at the host time
    double maxStep  = 0.0;   // between consecutive output samples
};

Rendered render(Session& session, Renderer& renderer, const Peer& peer, Micros hostTime,
                size_t frames = kRender) {
    std::vector<double> l(frames), r(frames);
    Rendered out;
    out.frames = renderer.receive(l.data(), r.data(), frames, session.state, double(kRate),
                                  hostTime, kQuantum);
    // The peer's frame at the host time: its sample clock runs from t0.
    const double frameAtHost = double((hostTime - peer.t0).count()) * double(kRate) / 1e6;
    for (size_t i = 0; i < out.frames; ++i) {
        const double want = std::sin(kTau * kHz * (frameAtHost + double(i)) / double(kRate));
        out.maxError = std::max(out.maxError, std::fabs(l[i] - want));
        if (i > 0) out.maxStep = std::max(out.maxStep, std::fabs(l[i] - l[i - 1]));
    }
    return out;
}

// As render(), against a session state the case has moved, with the stream
// expected `offFrames` earlier than the peer's clock says.
Rendered renderOn(const ableton::LinkAudio::SessionState& state, Renderer& renderer,
                  const Peer& peer, Micros hostTime, double offFrames) {
    std::vector<double> l(kRender), r(kRender);
    Rendered out;
    out.frames = renderer.receive(l.data(), r.data(), kRender, state, double(kRate), hostTime, kQuantum);
    const double frameAtHost = double((hostTime - peer.t0).count()) * double(kRate) / 1e6 - offFrames;
    for (size_t i = 0; i < out.frames; ++i) {
        const double want = std::sin(kTau * kHz * (frameAtHost + double(i)) / double(kRate));
        out.maxError = std::max(out.maxError, std::fabs(l[i] - want));
        if (i > 0) out.maxStep = std::max(out.maxStep, std::fabs(l[i] - l[i - 1]));
    }
    return out;
}

// The session state as a clock re-measurement leaves it. Link builds the
// state a client sees from the session's tempo and beat origin and a time
// origin put through the ghost transform (Controller.hpp): a new measurement
// against a peer moves that time origin and nothing else, so the beat at a
// host time moves while a buffer's stamp, a session beat, maps as before.
ableton::LinkAudio::SessionState remeasured(ableton::LinkAudio::SessionState state, Micros by) {
    auto& api = const_cast<ableton::link::ApiState&>(ableton::detail::linkApiState(state));
    api.timeline.timeOrigin += by;
    return state;
}

// The most two consecutive samples of this sine differ by, with a little room.
constexpr double kSineStep = 1.05 * kTau * kHz / double(kRate);
// Within a tenth of a frame: int16 quantisation, cubic interpolation between
// frames, and Link's whole microseconds, which cut a render's end short by
// up to one — a thirtieth of a frame, steered out in the next.
constexpr double kExact = 1e-2;

Micros framesLater(Micros t, double frames) {
    return t + Micros(static_cast<int64_t>(std::llround(1e6 * frames / double(kRate))));
}


// ── What else the stream carries ──────────────────────────────────────────
//
// A sine at a frequency no simple fraction of the rates, rendered for a while,
// and the loudest tone in what came out that is not that sine. A resampler
// that only looks a frame or two around the read position puts the stream's
// high frequencies back out as other tones, faint and inharmonic: bells over
// every hi-hat. Measured by an FFT under a Blackman-Harris window, which keeps
// the sine's own skirt narrow.

// Renders `frames` of output at `outRate`, consecutive renders of kRender, the
// peer publishing a second ahead of what is read. Against `state`, the
// session's or one the case has moved.
std::vector<double> renderRun(const ableton::LinkAudio::SessionState& state, Renderer& renderer,
                              Peer& peer, Micros& host, double outRate, size_t frames) {
    std::vector<double> l(kRender), r(kRender), out;
    out.reserve(frames + kRender);
    while (out.size() < frames) {
        const double readTo = double((host - peer.t0).count()) * double(kRate) / 1e6;
        while (double(peer.frame) < readTo + double(kRate)) peer.publish(1);
        const size_t got = renderer.receive(l.data(), r.data(), kRender, state, outRate, host, kQuantum);
        REQUIRE(got == kRender);
        out.insert(out.end(), l.begin(), l.end());
        host += Micros(static_cast<int64_t>(std::llround(1e6 * double(kRender) / outRate)));
    }
    out.resize(frames);
    return out;
}

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> step(std::cos(-kTau / double(len)), std::sin(-kTau / double(len)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// The loudest tone in `x` (a power of two long) that is not the sine at
// `hz`, in dB relative to the sine; and where it is, in Hz.
struct Spur { double db = 0.0; double hz = 0.0; };
Spur loudestOtherTone(const std::vector<double>& x, double rate, double hz) {
    const size_t n = x.size();
    std::vector<std::complex<double>> a(n);
    for (size_t k = 0; k < n; ++k) {
        const double p = kTau * double(k) / double(n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(p) + 0.14128 * std::cos(2 * p) - 0.01168 * std::cos(3 * p);
        a[k] = w * x[k];
    }
    fft(a);
    const auto sineBin = static_cast<size_t>(std::llround(hz / rate * double(n)));
    constexpr size_t kSkirt = 12;   // the window's main lobe and then some
    double sine = 0.0, other = 0.0;
    size_t otherBin = 0;
    for (size_t b = 1; b < n / 2; ++b) {
        const double m = std::abs(a[b]);
        if (b + kSkirt >= sineBin && b <= sineBin + kSkirt) { sine = std::max(sine, m); continue; }
        if (m > other) { other = m; otherBin = b; }
    }
    return { 20.0 * std::log10(other / sine), double(otherBin) * rate / double(n) };
}

// A high sine, no simple fraction of either rate, and how quietly anything
// else may come out with it: int16 on the wire is near -100 dB.
constexpr double kHighHz      = 12345.0;
constexpr double kCleanDb     = -90.0;
constexpr size_t kSpectrumLen = size_t{1} << 16;

}  // namespace

TEST_CASE("input renderer: a stream at another rate is converted without tones of its own",
          "[link-audio][renderer]") {
    // A peer at 48 kHz heard by an engine at 44.1: every frame resampled.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.hz = kHighHz;
    peer.publish(750);
    constexpr double kOutRate = 44100.0;
    Micros host = framesLater(peer.t0, 4800);
    renderRun(session.state, renderer, peer, host, kOutRate, 11025);   // settled
    const auto heard = renderRun(session.state, renderer, peer, host, kOutRate, kSpectrumLen);
    const auto spur = loudestOtherTone(heard, kOutRate, kHighHz);
    INFO("the loudest other tone: " << spur.db << " dB, at " << spur.hz << " Hz");
    CHECK(spur.db < kCleanDb);
    CHECK(renderer.resyncs() == 0);
}

TEST_CASE("input renderer: a stream being steered is bent without tones of its own",
          "[link-audio][renderer]") {
    // The timeline moved 20 ms under the stream (a clock re-measurement): it
    // is read a hundredth fast for two seconds, and every frame of that is
    // between the stream's own.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.hz = kHighHz;
    peer.publish(750);
    Micros host = framesLater(peer.t0, 4800);
    renderRun(session.state, renderer, peer, host, double(kRate), 4800);
    const auto moved = remeasured(session.state, Micros(-20'000));
    const auto heard = renderRun(moved, renderer, peer, host, double(kRate), kSpectrumLen);
    const auto spur = loudestOtherTone(heard, double(kRate), kHighHz * 1.01);
    INFO("the loudest other tone: " << spur.db << " dB, at " << spur.hz << " Hz");
    CHECK(spur.db < kCleanDb);
    CHECK(renderer.resyncs() == 0);
}

// The converter reads the stream ahead of where it is, further the lower the
// rate it is read into, and a stream is rendered only as far as that reach
// has arrived. Run short, libsamplerate takes the stream as ended and pads it
// with silence. At every rate an engine runs at, steered as hard as it is
// ever steered: every render whole, and the place never lost.
TEST_CASE("input renderer: a stream is read at any engine rate without running the converter short",
          "[link-audio][renderer]") {
    const double outRate = GENERATE(16000.0, 22050.0, 44100.0, 96000.0, 192000.0);
    CAPTURE(outRate);
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);
    Micros host = framesLater(peer.t0, 4800);
    renderRun(session.state, renderer, peer, host, outRate, size_t(outRate / 4));
    const auto moved = remeasured(session.state, Micros(-20'000));   // steered a hundredth fast
    renderRun(moved, renderer, peer, host, outRate, size_t(outRate));
    CHECK(renderer.resyncs() == 0);
}

TEST_CASE("input renderer: a stream is rendered from where its stamps put it",
          "[link-audio][renderer]") {
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);   // a second

    const auto at = render(session, renderer, peer, framesLater(peer.t0, 4800));
    CHECK(at.frames == kRender);
    CHECK(at.maxError < kExact);
    CHECK(renderer.resyncs() == 0);
    CHECK(renderer.warps() == 0);
}

TEST_CASE("input renderer: consecutive renders are one continuous stream",
          "[link-audio][renderer]") {
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 60; ++block) {   // 640 ms
        const auto at = render(session, renderer, peer, host);
        INFO("block " << block);
        REQUIRE(at.frames == kRender);
        CHECK(at.maxError < kExact);
        CHECK(at.maxStep < kSineStep);
        host = framesLater(host, double(kRender));
    }
    CHECK(renderer.resyncs() == 0);
    CHECK(renderer.warps() == 0);
}

TEST_CASE("input renderer: stamps that jitter by under a frame are one stream, not holes",
          "[link-audio][renderer]") {
    // A real publisher's stamps are rounded through beats and microseconds:
    // consecutive buffers abut to within a frame, not exactly. That is one
    // continuous stream, rendered by its frames; a hole is a millisecond or
    // more, a buffer that never came.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.jitterFrames = 0.9;
    peer.publish(750);

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 60; ++block) {
        const auto at = render(session, renderer, peer, host);
        INFO("block " << block);
        REQUIRE(at.frames == kRender);
        CHECK(at.maxStep < kSineStep);
        host = framesLater(host, double(kRender));
    }
    CHECK(renderer.resyncs() == 0);
    CHECK(renderer.warps() == 0);
}

TEST_CASE("input renderer: a few frames' disagreement is steered over the render, not a lost place",
          "[link-audio][renderer]") {
    // Two sample clocks never quite agree; the block stamp steers. A render
    // asked for a couple of frames later than the stream's own clock says is
    // bent to meet it, and the place is kept.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);

    Micros host = framesLater(peer.t0, 4800);
    render(session, renderer, peer, host);
    host = framesLater(host, double(kRender) + 2.0);
    const auto bent = render(session, renderer, peer, host);
    CHECK(bent.frames == kRender);
    CHECK(bent.maxStep < kSineStep * 1.1);
    CHECK(renderer.resyncs() == 0);
}

TEST_CASE("input renderer: a step of a couple of milliseconds is steered out, not skipped",
          "[link-audio][renderer]") {
    // Link re-measures the clock offset to a peer every so often and corrects
    // the session timeline by a millisecond or two: the target moves under
    // the stream in one go. Bent into one render that is a yelp; skipped it
    // is a click. It is steered: each render bends by a bounded share toward
    // the target, and the stream is back in place within a second with
    // nothing heard.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 10; ++block) {
        REQUIRE(render(session, renderer, peer, host).frames == kRender);
        host = framesLater(host, double(kRender));
    }
    host = framesLater(host, 96.0);   // 2 ms on, in one go
    double worstStep = 0.0;
    for (int block = 0; block < 60; ++block) {   // 640 ms
        const auto at = render(session, renderer, peer, host);
        INFO("block " << block << " after the step");
        REQUIRE(at.frames == kRender);
        worstStep = std::max(worstStep, at.maxStep);
        host = framesLater(host, double(kRender));
    }
    CHECK(worstStep < kSineStep * 1.05);   // bent by a hundredth at most
    CHECK(renderer.resyncs() == 0);
    CHECK(renderer.warps() == 0);
    const auto settled = render(session, renderer, peer, host);
    CHECK(settled.maxError < kExact);   // back in place
}

TEST_CASE("input renderer: after the host has gone on without it, the stream rejoins the present",
          "[link-audio][renderer]") {
    // The ring was read dry for 100 ms — the worker starved, the device
    // stalled — and the audio thread heard silence. What was missed is gone:
    // the next render is the audio of now, not the missed audio played five
    // times too fast to catch up, which is heard as a chirp and a burst of
    // clicks. Finding the place again is a resync; bending to it is not.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 10; ++block) {
        REQUIRE(render(session, renderer, peer, host).frames == kRender);
        host = framesLater(host, double(kRender));
    }
    host = framesLater(host, 4800.0);   // 100 ms nobody asked for
    const auto rejoined = render(session, renderer, peer, host);
    CHECK(rejoined.frames == kRender);
    CHECK(rejoined.maxError < kExact);
    CHECK(rejoined.maxStep < kSineStep);
    CHECK(renderer.resyncs() == 1);
    CHECK(renderer.warps() == 0);

    // And on from there, continuous.
    host = framesLater(host, double(kRender));
    const auto on = render(session, renderer, peer, host);
    CHECK(on.frames == kRender);
    CHECK(on.maxError < kExact);
    CHECK(renderer.resyncs() == 1);
    CHECK(renderer.warps() == 0);
}

TEST_CASE("input renderer: a re-measured clock that moves the timeline tens of milliseconds is steered to, not a lost place",
          "[link-audio][renderer]") {
    // Over WiFi, with a second peer in the session, Link's measurements of the
    // clock offset move the timeline under a stream by tens of milliseconds
    // at a time, not one or two. Nothing was missed and nothing stalled: the
    // stream is whole, the target moved. Treated as a lost place, that would
    // be a resync and a hole in the audio at every measurement. It is steered
    // to, as a small step is, and the stream is in its new place within a few
    // seconds with nothing heard.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    // A second ahead, and a render's worth more with every render, as a peer
    // publishes: the queue holds under three seconds, not the whole case.
    peer.publish(750);
    constexpr size_t kPerRender = kRender / kPeerBlock;

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 10; ++block) {
        REQUIRE(render(session, renderer, peer, host).frames == kRender);
        peer.publish(kPerRender);
        host = framesLater(host, double(kRender));
    }
    // Either way: the measurement can put the session's clock ahead of where
    // it was thought to be, or behind.
    const double kMovedMs = GENERATE(20.0, -20.0);
    INFO("the timeline moved " << kMovedMs << " ms");
    const auto moved = remeasured(session.state, Micros(static_cast<int64_t>(kMovedMs * 1000)));
    const double movedFrames = kMovedMs * double(kRate) / 1000.0;

    double worstStep = 0.0;
    for (int block = 0; block < 240; ++block) {   // 2.5 s
        const auto at = renderOn(moved, renderer, peer, host, movedFrames);
        INFO("block " << block << " after the timeline moved");
        REQUIRE(at.frames == kRender);
        worstStep = std::max(worstStep, at.maxStep);
        peer.publish(kPerRender);
        host = framesLater(host, double(kRender));
    }
    CHECK(worstStep < kSineStep * 1.05);   // bent by a hundredth at most, never stepped
    CHECK(renderer.resyncs() == 0);
    CHECK(renderer.warps() == 0);
    const auto settled = renderOn(moved, renderer, peer, host, movedFrames);
    CHECK(settled.maxError < kExact);   // in its new place
}

TEST_CASE("input renderer: a clock re-measured again and again is one stream, never a lost place",
          "[link-audio][renderer]") {
    // Over WiFi with a second peer, Link re-measures the clock to its peers
    // over and over, and the timeline swings under the stream, one way and
    // back, tens of milliseconds at a time: several swings a second. Each
    // swing treated as a lost place would be a resync. The stream is steered
    // through every swing and never stepped, and once the timeline is back
    // where it began the stream is back in its place.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);
    constexpr size_t kPerRender = kRender / kPeerBlock;

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 10; ++block) {
        REQUIRE(render(session, renderer, peer, host).frames == kRender);
        peer.publish(kPerRender);
        host = framesLater(host, double(kRender));
    }
    // Where each measurement leaves the timeline, from where it began, in ms;
    // the last puts it back.
    const double kMovesMs[] = { 15.0, -5.0, 20.0, 0.0, -15.0, 10.0, 0.0 };
    double worstStep = 0.0;
    for (size_t m = 0; m < std::size(kMovesMs); ++m) {
        const double movedMs = kMovesMs[m];
        const auto moved = remeasured(session.state, Micros(static_cast<int64_t>(movedMs * 1000)));
        const double movedFrames = movedMs * double(kRate) / 1000.0;
        // ~0.4 s at each; at the last, back where it began, long enough to settle.
        const int renders = (m + 1 == std::size(kMovesMs)) ? 400 : 40;
        for (int block = 0; block < renders; ++block) {
            const auto at = renderOn(moved, renderer, peer, host, movedFrames);
            INFO("timeline at " << movedMs << " ms, block " << block);
            REQUIRE(at.frames == kRender);
            worstStep = std::max(worstStep, at.maxStep);
            peer.publish(kPerRender);
            host = framesLater(host, double(kRender));
        }
    }
    CHECK(worstStep < kSineStep * 1.05);   // bent by a hundredth at most, never stepped
    CHECK(renderer.resyncs() == 0);
    CHECK(renderer.warps() == 0);
    const auto settled = render(session, renderer, peer, host);
    CHECK(settled.maxError < kExact);   // back in its place
}

TEST_CASE("input renderer: a timeline that jumps further than a clock measurement moves it is found again at once",
          "[link-audio][renderer]") {
    // Half a second is no measurement but the session itself jumping, and the
    // stream is found again where the timeline now is, not bent toward it for
    // a minute. Later, so the audio there has arrived; half a second earlier
    // was played and let go, and is found again when the target reaches what
    // is still held.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(750);
    constexpr size_t kPerRender = kRender / kPeerBlock;

    Micros host = framesLater(peer.t0, 4800);
    for (int block = 0; block < 10; ++block) {
        REQUIRE(render(session, renderer, peer, host).frames == kRender);
        peer.publish(kPerRender);
        host = framesLater(host, double(kRender));
    }
    constexpr double kJumpedMs = -500.0;
    const auto jumped = remeasured(session.state, Micros(static_cast<int64_t>(kJumpedMs * 1000)));
    const double jumpedFrames = kJumpedMs * double(kRate) / 1000.0;
    const auto rejoined = renderOn(jumped, renderer, peer, host, jumpedFrames);
    CHECK(rejoined.frames == kRender);
    CHECK(rejoined.maxError < kExact);
    CHECK(renderer.resyncs() == 1);
    CHECK(renderer.warps() == 0);
}

TEST_CASE("input renderer: a hole in the peer's stream is nothing to render, then the stream resumes",
          "[link-audio][renderer]") {
    // The peer's callback stalled for 100 ms: those frames were never made,
    // and its stamps go on from the present. Asked inside the hole there is
    // nothing yet; asked after it, the stream as it resumes, whole.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    peer.publish(150);   // 200 ms
    const uint64_t holeAt = peer.frame;
    peer.skip(4800);     // 100 ms never made
    peer.publish(600);   // 800 ms more

    const uint64_t holeEnd = holeAt + 4800;
    uint64_t at = 4800;
    for (; at + kRender <= holeAt - 2 * kRender; at += kRender) {   // well before the hole
        REQUIRE(render(session, renderer, peer, framesLater(peer.t0, double(at))).frames == kRender);
    }
    // Up to the hole and into it: everything before the hole is rendered, to
    // the interpolator's reach short of its edge, and nothing from inside it.
    const uint64_t edge = at;
    size_t beforeHole = 0;
    for (; at < holeEnd; at += kRender) {
        beforeHole += render(session, renderer, peer, framesLater(peer.t0, double(at))).frames;
    }
    CHECK(beforeHole == holeAt - edge - Renderer::reach(kRate, double(kRate)));
    CHECK(renderer.warps() == 0);

    // Past the hole: the stream as the peer resumed it, and on from there.
    const auto resumed = render(session, renderer, peer, framesLater(peer.t0, double(at)));
    CHECK(resumed.frames == kRender);
    CHECK(resumed.maxError < kExact);
    CHECK(resumed.maxStep < kSineStep);
    CHECK(renderer.resyncs() == 1);
    CHECK(renderer.warps() == 0);
    at += kRender;
    const auto on = render(session, renderer, peer, framesLater(peer.t0, double(at)));
    CHECK(on.frames == kRender);
    CHECK(on.maxError < kExact);
    CHECK(renderer.resyncs() == 1);
}


TEST_CASE("input renderer: a short hole asked across a device's block at a time is found again on its far side",
          "[link-audio][renderer]") {
    // The endpoint asks again as each device block is read. Inside a hole
    // nothing renders while the host goes on a block at a time, each step
    // smaller than the slack a render that follows on is allowed. The steps
    // add up: the host went on without the stream, which is a place lost, and
    // the stream is found again on the far side as soon as it is there, not
    // steered across the hole as a timeline that moved would be.
    Session session;
    Renderer renderer(session.link);
    renderer.setLatencySeconds(0.0);
    Peer peer{session, renderer, Micros(1'000'000'000)};
    constexpr size_t kDeviceBlock = 64;
    peer.publish(150);   // 200 ms
    const uint64_t holeAt = peer.frame;
    peer.skip(23 * kDeviceBlock);   // 30.7 ms never made
    peer.publish(600);
    const uint64_t holeEnd = holeAt + 23 * kDeviceBlock;

    uint64_t at = 4800;
    for (; at + kDeviceBlock + Renderer::reach(kRate, double(kRate)) < holeAt; at += kDeviceBlock) {   // up to the hole's edge
        REQUIRE(render(session, renderer, peer, framesLater(peer.t0, double(at)), kDeviceBlock).frames
                == kDeviceBlock);
    }
    for (; at < holeEnd; at += kDeviceBlock) {   // into it and across: the edge, then nothing
        render(session, renderer, peer, framesLater(peer.t0, double(at)), kDeviceBlock);
    }
    const auto resumed = render(session, renderer, peer, framesLater(peer.t0, double(at)), kDeviceBlock);
    CHECK(resumed.frames == kDeviceBlock);
    CHECK(resumed.maxError < kExact);
    CHECK(renderer.resyncs() == 1);
    CHECK(renderer.warps() == 0);
}
#endif  // CLOCKWORK_LINK_AUDIO
