// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_link_audio_host.cpp — the Link Audio host, next to the clock, not in it.
 *
 * LinkAudioHost is the engine's owner of the audio half of Link: the publish
 * gate, peer channels, input subscriptions, sinks, and the block stamp in
 * Link's own clock domain. ClockworkClock keeps the Link *session* (tempo,
 * transport, peers — clock work); the host borrows that session and hangs
 * off the clock's visibility transitions as a listener, so Link and Link
 * Audio still go up and down together, in order.
 */
#include <catch2/catch_test_macros.hpp>

#include "clock/ClockworkClock.h"
#include "native/LinkAudioBridge.h"
#include "native/LinkAudioHost.h"
#include "shared_memory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

bool flagSet(const ClockworkClock& clock, uint32_t mask) {
    return (clock.state()->flags.load(std::memory_order_relaxed) & mask) != 0;
}

// Records the clock's visibility protocol as it happens, with what the
// session looked like at each call.
struct Recorder : ClockworkClock::LinkVisibilityListener {
    ClockworkClock& clock;
    std::vector<std::string> log;
    explicit Recorder(ClockworkClock& c) : clock(c) {}
    void linkWillChangeVisibility() override {
        log.push_back(std::string("will:") + (clock.isLinkEnabled() ? "on" : "off"));
    }
    void linkDidEnable() override {
        log.push_back(std::string("enabled:") + (clock.isLinkEnabled() ? "on" : "off"));
    }
};

} // namespace

TEST_CASE("link audio host: block stamp is sample-locked in Link's domain, 0 without Link",
          "[clock][link-audio]") {
    ClockworkClock clock;
    LinkAudioHost host(clock);
    const double sr = 48000.0;
    clock.resetAudioThreadTime(0.0, sr);
    host.resetBlockClock();

    const int64_t a = host.blockHostMicros(0.0, sr);
    const int64_t b = host.blockHostMicros(64.0, sr);
    const int64_t c = host.blockHostMicros(128.0, sr);
#if CLOCKWORK_LINK
    // One block is 64/48000 s = 1333.3 us; the IIR moves the base by a
    // fraction of the (microsecond-scale) wake jitter, never by a block.
    CHECK(a > 0);
    CHECK(b - a >= 1300); CHECK(b - a <= 1370);
    CHECK(c - b >= 1300); CHECK(c - b <= 1370);
#else
    CHECK(a == 0);
    CHECK(b == 0);
    CHECK(c == 0);
#endif
}

TEST_CASE("link audio host: the publish gate is mirrored into the clock state",
          "[clock][link-audio]") {
    ClockworkClock clock;
    LinkAudioHost host(clock);
    CHECK_FALSE(host.isPublishEnabled());
    CHECK_FALSE(flagSet(clock, SC_FLAG_LINK_AUDIO_PUBLISH));

    host.setPublishEnabled(true);
    CHECK(host.isPublishEnabled());
    CHECK(flagSet(clock, SC_FLAG_LINK_AUDIO_PUBLISH));

    host.setPublishEnabled(false);
    CHECK_FALSE(host.isPublishEnabled());
    CHECK_FALSE(flagSet(clock, SC_FLAG_LINK_AUDIO_PUBLISH));
}

TEST_CASE("link audio host: the clock tells its listener around every visibility change",
          "[clock][link-audio]") {
    ClockworkClock clock;
    Recorder rec(clock);
    clock.setLinkVisibilityListener(&rec);
    using V = ClockworkClock::LinkVisibility;

    clock.setLinkVisibility(V::LoopbackOnly);
    clock.setLinkVisibility(V::Off);
    clock.setLinkVisibility(V::Off);   // no transition, no call
#if CLOCKWORK_LINK
    // Bring-up: the listener drops its substrate-bound state before the
    // session is touched, and gets the session back only once it is up.
    // Tear-down: told before, never "enabled" after.
    const std::vector<std::string> want{"will:off", "enabled:on", "will:on"};
    CHECK(rec.log == want);
#else
    // No Link compiled: visibility is clamped to Off, so nothing changes.
    CHECK(rec.log.empty());
#endif

    // A cleared listener is left alone.
    clock.setLinkVisibilityListener(nullptr);
    clock.setLinkVisibility(V::LoopbackOnly);
    clock.setLinkVisibility(V::Off);
#if CLOCKWORK_LINK
    CHECK(rec.log.size() == 3);
#endif
}

TEST_CASE("link audio host: channels, inputs and sinks are empty and refuse until a mesh exists",
          "[clock][link-audio]") {
    ClockworkClock clock;
    LinkAudioHost host(clock);
    CHECK(host.listChannels().empty());
    CHECK(host.listInputs().empty());
    CHECK(host.listSinks().empty());
    CHECK_FALSE(host.addInput("nobody", "main", 0));
    CHECK_FALSE(host.setInputLatencySeconds("nobody", "main", 0.1));
    host.removeInput("nobody", "main");
    host.clearInputs();
    host.removeSink("aux");
    CHECK(host.listSinks().empty());

    // Publishing with nobody subscribed ships nothing.
    float silence[64] = {0.0f};
    CHECK_FALSE(host.publishAudioBlock(silence, silence, 64, 48000, 0));
}

// ── The width a subscription is measured against ─────────────────────────────
//
// channelPairFits is the whole of that decision, and it is pure, so it is
// pinned here exhaustively rather than only through a bridge that needs a
// mesh. It runs in BOTH Link configurations: the function lives outside the
// CLOCKWORK_LINK_AUDIO compile shapes precisely so a Link-off build still
// checks it.
//
// The width passed in is the ALLOCATED one — device inputs plus the lanes
// reserved above them. Handing it the device's width instead is the bug this
// pins: a client subscribes into a reserved lane, which is above every device
// channel, so a device-width check refuses everything. See
// test_reserved_lanes.cpp, which measures both numbers against a real boot.

TEST_CASE("link audio: a stereo pair needs both channels inside the width",
          "[link-audio]") {
    // A pair occupies busIdx and busIdx+1, so the last pair that fits a width
    // of n starts at n-2.
    CHECK(link_audio::channelPairFits(0, 2));
    CHECK_FALSE(link_audio::channelPairFits(1, 2));   // 1/2, and 2 is past the end
    CHECK(link_audio::channelPairFits(0, 3));
    CHECK(link_audio::channelPairFits(1, 3));
    CHECK_FALSE(link_audio::channelPairFits(2, 3));

    // A wide engine: everything up to the last pair.
    CHECK(link_audio::channelPairFits(30, 40));
    CHECK(link_audio::channelPairFits(38, 40));
    CHECK_FALSE(link_audio::channelPairFits(39, 40));
    CHECK_FALSE(link_audio::channelPairFits(40, 40));
    CHECK_FALSE(link_audio::channelPairFits(41, 40));
}

TEST_CASE("link audio: no width admits any pair", "[link-audio]") {
    // Before the engine has a format there is nothing to fit into, and a
    // width of one cannot hold a stereo pair however it is indexed.
    CHECK_FALSE(link_audio::channelPairFits(0, 0));
    CHECK_FALSE(link_audio::channelPairFits(1, 0));
    CHECK_FALSE(link_audio::channelPairFits(0, 1));
}

TEST_CASE("link audio: a busIdx near the integer ceiling cannot wrap into a pass",
          "[link-audio]") {
    // Written as `busIdx + 1 >= inChannels` this was a real hazard: at
    // UINT32_MAX the addition wraps to 0, which is < any width, and a wildly
    // out-of-range subscription would have been ACCEPTED and then silently
    // skipped for ever by pull_port_sources.
    CHECK_FALSE(link_audio::channelPairFits(UINT32_MAX, 40));
    CHECK_FALSE(link_audio::channelPairFits(UINT32_MAX, UINT32_MAX));
    CHECK_FALSE(link_audio::channelPairFits(UINT32_MAX - 1, UINT32_MAX));
    // The genuinely last pair in the space still fits.
    CHECK(link_audio::channelPairFits(UINT32_MAX - 2, UINT32_MAX));
}
