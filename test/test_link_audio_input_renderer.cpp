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
 *     resumes; one resync, no warp.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/LinkAudioBridge.h"   // decides CLOCKWORK_LINK_AUDIO

#if CLOCKWORK_LINK_AUDIO

#include "native/vendor/LinkAudioInputRenderer.hpp"

#include <ableton/LinkAudio.hpp>

#include <chrono>
#include <cmath>
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

    static double sine(uint64_t frame) {
        return std::sin(kTau * kHz * double(frame) / double(kRate));
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
            bh.info.sessionBeatTime = session.beatAt(timeOf(frame));
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

// The most two consecutive samples of this sine differ by, with a little room.
constexpr double kSineStep = 1.05 * kTau * kHz / double(kRate);
// Within a tenth of a frame: int16 quantisation, cubic interpolation between
// frames, and Link's whole microseconds, which cut a render's end short by
// up to one — a thirtieth of a frame, steered out in the next.
constexpr double kExact = 1e-2;

Micros framesLater(Micros t, double frames) {
    return t + Micros(static_cast<int64_t>(std::llround(1e6 * frames / double(kRate))));
}

}  // namespace

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

TEST_CASE("input renderer: a few frames' disagreement is steered over the render, not a lost place",
          "[link-audio][renderer]") {
    // Two sample clocks never quite agree; the block stamp steers. A render
    // asked for a couple of frames later than the stream's own clock says is
    // bent to meet it, as upstream always did, and the place is kept.
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
    // two frames of its edge (the interpolator's reach), and nothing from
    // inside it.
    const uint64_t edge = at;
    size_t beforeHole = 0;
    for (; at < holeEnd; at += kRender) {
        beforeHole += render(session, renderer, peer, framesLater(peer.t0, double(at))).frames;
    }
    CHECK(beforeHole == holeAt - edge - 2);
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

#endif  // CLOCKWORK_LINK_AUDIO
