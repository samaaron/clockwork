// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_reserved_lanes.cpp — the channels above the device, and what reaches
 * the DSP through them.
 *
 * This is where a Link peer's audio lands. clockwork_reserve_lanes widens the
 * engine's channel counts past whatever the device supplies, and a source port
 * attached to one of those lanes is pulled into the DSP's inputs by
 * pull_port_sources, exactly as a WAV streaming off disk is. Link contributes
 * only the thread that fills the port, so everything here is deterministic and
 * needs no peer, no mesh and no network.
 *
 * A BINARY OF ITS OWN, because clockwork is a process singleton: reserving
 * lanes changes the geometry for the whole process, and every case in
 * clockwork_tests is written against kInChannels/kOutChannels. A second
 * geometry needs a second process.
 *
 * What this pins that nothing else did: a subscription is validated against
 * the width the DSP ALLOCATED, not the width the device happens to have. The
 * two are different by construction — a lane sits above every device channel —
 * so measuring against the device refuses every request a client can make.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"
#include "clockwork_ports.h"
#include "clockwork_port_bus.h"
#include "native/LinkAudioBridge.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

extern "C" {
    int get_audio_num_input_buses();
    int get_audio_num_output_buses();
}

namespace {

// Everything a case opens goes away at its end, pass or fail.
struct Ports {
    ~Ports() { clockwork_port_bus_detach_all(); clockwork_port_close_all(); }
};

// Frame f, channel c. Integers below 2^24 are exact in float, so every
// assertion is an equality rather than a tolerance.
float sample(uint64_t f, uint32_t c) { return static_cast<float>(f * 4 + c) / 8.0f; }

// The placeholder's echo mode: output channel c is input channel c, verbatim.
// That is the only window onto what pull_port_sources actually delivered.
void setEcho(bool on) {
    const auto m = on ? osc_test::message("/dummy/echo")
                      : osc_test::message("/dummy/pulse", 10, 500);
    REQUIRE(lanes_test::ingress(m.ptr(), m.size(), 0));
    lanes_test::tick();
    lanes_test::drainRt();
}

} // namespace

TEST_CASE("reserved lanes: the base sits above every device channel", "[lanes][reserved]") {
    lanes_test::boot();
    REQUIRE(lanes_test::kReservedLanes > 0);

    const uint32_t base = clockwork_lane_base();
    REQUIRE(clockwork_reserved_lanes() == lanes_test::kReservedLanes);

    // Above the device, in both directions. This is the property the whole
    // scheme rests on: a lane index can never collide with a device channel.
    REQUIRE(base >= lanes_test::kInChannels);
    REQUIRE(base >= lanes_test::kOutChannels);

    // And the engine allocated for them.
    const uint32_t allocatedIn  = static_cast<uint32_t>(get_audio_num_input_buses());
    const uint32_t allocatedOut = static_cast<uint32_t>(get_audio_num_output_buses());
    REQUIRE(allocatedIn  == base + lanes_test::kReservedLanes);
    REQUIRE(allocatedOut == base + lanes_test::kReservedLanes);

    // The device's own width is strictly smaller — the two numbers a
    // subscription could be measured against, and they disagree.
    REQUIRE(allocatedIn > lanes_test::kInChannels);
}

TEST_CASE("reserved lanes: a subscription is measured against the allocated width",
          "[lanes][reserved][link-audio]") {
    lanes_test::boot();
    const uint32_t base        = clockwork_lane_base();
    const uint32_t allocatedIn = static_cast<uint32_t>(get_audio_num_input_buses());

    // A stereo pair at the lane base is exactly what a client subscribes to.
    // Against the allocated width it fits; against the DEVICE width it does
    // not, and measuring against the device is what refused every real
    // subscription.
    REQUIRE(link_audio::channelPairFits(base, allocatedIn));
    REQUIRE_FALSE(link_audio::channelPairFits(base, lanes_test::kInChannels));

    // The top of the range: the last pair fits, one past it does not.
    REQUIRE(link_audio::channelPairFits(allocatedIn - 2, allocatedIn));
    REQUIRE_FALSE(link_audio::channelPairFits(allocatedIn - 1, allocatedIn));
    REQUIRE_FALSE(link_audio::channelPairFits(allocatedIn, allocatedIn));
}

TEST_CASE("reserved lanes: a source port on a lane reaches the DSP's inputs",
          "[lanes][reserved]") {
    const uint32_t bl   = lanes_test::boot();
    const uint32_t base = clockwork_lane_base();
    Ports guard;
    setEcho(true);

    const ClockworkPort p =
        clockwork_port_open("peer-audio", kClockworkPortSource, 2, 4096);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, base) == 1);

    // One block's worth, interleaved, distinguishable per channel and per
    // frame so a half-copy, a channel swap or an offset all show up.
    std::vector<float> interleaved(static_cast<size_t>(bl) * 2);
    for (uint32_t f = 0; f < bl; ++f)
        for (uint32_t c = 0; c < 2; ++c) interleaved[f * 2 + c] = sample(f, c);
    REQUIRE(clockwork_port_produce(p, interleaved.data(), bl) == bl);
    REQUIRE(clockwork_port_readable(p) == bl);

    lanes_test::tick();

    // Echo puts input channel c on output channel c, so the lane's two
    // channels are readable straight off the staging buffer.
    const float* out = clockwork_audio_out();
    REQUIRE(out != nullptr);
    for (uint32_t f = 0; f < bl; ++f) {
        REQUIRE(out[static_cast<size_t>(base) * bl + f]       == sample(f, 0));
        REQUIRE(out[static_cast<size_t>(base + 1) * bl + f]   == sample(f, 1));
    }

    // The port was drained by the pull, not merely read past.
    REQUIRE(clockwork_port_readable(p) == 0);

    setEcho(false);
}

TEST_CASE("reserved lanes: a port attached past the top is silent, not out of bounds",
          "[lanes][reserved]") {
    const uint32_t bl          = lanes_test::boot();
    const uint32_t allocatedIn = static_cast<uint32_t>(get_audio_num_input_buses());
    Ports guard;
    setEcho(true);

    // Attaching binds whatever it is given — the bus table takes no view on
    // the engine's width. pull_port_sources is what declines to read it, and
    // the point of this case is that it declines rather than running off the
    // end of the staging buffer.
    const ClockworkPort p =
        clockwork_port_open("past-the-end", kClockworkPortSource, 2, 4096);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, allocatedIn + 4) == 1);

    std::vector<float> interleaved(static_cast<size_t>(bl) * 2, 0.5f);
    REQUIRE(clockwork_port_produce(p, interleaved.data(), bl) == bl);

    lanes_test::tick();

    // Nothing was consumed: the binding starts past the end and is skipped.
    REQUIRE(clockwork_port_readable(p) == bl);

    setEcho(false);
}
