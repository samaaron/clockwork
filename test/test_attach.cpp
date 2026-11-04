// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_attach.cpp — a core's lifecycle through the lanes ABI: made, booted
 * with its host's geometry, reconfigured by that host, closed.
 *
 * Here, in clockwork_attach_tests, because each case makes the process's
 * core and closes it, and clockwork_tests' fixture holds one for the whole
 * binary. Each case leaves no core live.
 */
#include "lanes/lanes.h"
#include "shared_memory.h"
#include "clockwork_ports.h"
#include "clockwork_port_bus.h"
#include "clockwork_event_sink.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {
alignas(16) uint8_t gArena[1 << 20];
constexpr double   kRate  = 48000.0;
constexpr uint32_t kBlock = 128;

// A core for the length of a case.
struct Core {
    ClockworkCore* core = nullptr;
    Core() {
        ClockworkStatus st = CLOCKWORK_OK;
        core = clockwork_core_new(&st);
        REQUIRE(core != nullptr);
        REQUIRE(st == CLOCKWORK_OK);
    }
    ~Core() { clockwork_core_close(core); }
};
}

TEST_CASE("core: booted with its host's geometry, and a reconfigure is that host's device switch",
          "[attach][core]") {
    Core c;
    clockwork_init(c.core, kRate, kBlock, /*in*/ 2, /*out*/ 8, /*verbosity*/ 0,
                   gArena, sizeof gArena, nullptr, 0, 0);
    REQUIRE(clockwork_sample_rate(c.core) == kRate);
    REQUIRE(clockwork_block_size(c.core) == kBlock);
    REQUIRE(clockwork_lanes_base(c.core) != nullptr);

    // The same host's device switch: routine, and applied.
    clockwork_init(c.core, kRate, 256, 2, 8, 0, gArena, sizeof gArena, nullptr, 0, 0);
    REQUIRE(clockwork_block_size(c.core) == 256);
}

TEST_CASE("core: closing it ends its engine, and init and tick refuse it after", "[attach][core]") {
    ClockworkCore* closed = nullptr;
    {
        Core c;
        closed = c.core;
        clockwork_init(c.core, kRate, kBlock, 2, 2, 0, gArena, sizeof gArena, nullptr, 0, 0);
        REQUIRE(clockwork_sample_rate(c.core) == kRate);
    }
    // Closed, the engine has ended: no instance, no arena.
    CHECK(clockwork_sample_rate(closed) == 0.0);
    CHECK(clockwork_lanes_base(closed) == nullptr);
    // Refused, so nothing comes up.
    clockwork_init(closed, 44100.0, 512, 2, 2, 0, gArena, sizeof gArena, nullptr, 0, 0);
    CHECK(clockwork_sample_rate(closed) == 0.0);
    CHECK(clockwork_lanes_base(closed) == nullptr);
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    CHECK(clockwork_tick(closed, &time, 2, 2) == CLOCKWORK_TICK_NONE);
    CHECK(clockwork_tick(nullptr, &time, 2, 2) == CLOCKWORK_TICK_NONE);
}

// A core whose host is the far end of its chain is the sinks' MIDI endpoint
// (clockwork_host_forward): the emitter the sinks call is a record in the
// core's state, so it goes when the core does, and no MIDI sink opened after
// reaches a core that has gone.
TEST_CASE("core: closing a core whose host takes MIDI leaves no sink going to it", "[attach][core][sinks]") {
    {
        Core c;
        clockwork_init(c.core, kRate, kBlock, 2, 2, 0, gArena, sizeof gArena, nullptr, 0, 0);
        clockwork_host_forward(c.core, 1);
        const ClockworkSink sink = clockwork_sink_open(clockwork_core_sinks(c.core), kClockworkSinkMidi, "a port", 64);
        REQUIRE(sink != CLOCKWORK_SINK_NONE);   // the host's
        clockwork_sink_close(clockwork_core_sinks(c.core), sink);
    }
    // The next core's host has said nothing, and there is no MIDI subsystem
    // in this binary: nothing to open onto.
    Core next;
    clockwork_init(next.core, kRate, kBlock, 2, 2, 0, gArena, sizeof gArena, nullptr, 0, 0);
    CHECK(clockwork_sink_open(clockwork_core_sinks(next.core), kClockworkSinkMidi, "a port", 64) == CLOCKWORK_SINK_NONE);
}

// A reconfigure the guest takes can widen it with no rebuild: the block's
// buffers follow, so every output channel lies where a host reads it and
// none of them is the input a host fills.
TEST_CASE("core: a reconfigure that widens the guest lays the block's buffers out again", "[attach][core][staging]") {
    Core c;
    clockwork_init(c.core, kRate, kBlock, /*in*/ 2, /*out*/ 2, 0, gArena, sizeof gArena, nullptr, 0, 0);
    REQUIRE(clockwork_out_channels(c.core) == 2);
    REQUIRE(clockwork_guest_reconfigure(c.core, kRate, /*in*/ 2, /*out*/ 6) == 1);
    REQUIRE(clockwork_out_channels(c.core) == 6);

    const float* out = clockwork_audio_out(c.core);
    const float* in  = clockwork_audio_in(c.core);
    REQUIRE(out != nullptr);
    REQUIRE(in != nullptr);
    CHECK(in >= out + 6 * kBlock);

    // A block: the dummy's channels alternate left and right, so the four
    // new ones repeat the first two.
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    REQUIRE(clockwork_tick(c.core, &time, 6, 2) == CLOCKWORK_TICK_RENDERED);
    CHECK(out[0] != 0.0f);
    for (uint32_t ch = 2; ch < 6; ++ch)
        for (uint32_t f = 0; f < kBlock; ++f) {
            INFO("channel " << ch << ", frame " << f);
            REQUIRE(out[ch * kBlock + f] == out[(ch % 2) * kBlock + f]);
        }
}

// ── A rebuild does not forget what is bound ─────────────────────────────────
//
// The map is state, not a message, so a rebuild needs no replay of the
// attachments; but the device widths a rebuild brings must not trample it.

TEST_CASE("channel map: a rebuild keeps the streams bound across it", "[attach][channel-map]") {
    static unsigned char arena2[1 << 20];
    Core c;
    clockwork_init(c.core, kRate, kBlock, 2, 8, 0, arena2, sizeof arena2, nullptr, 0, 0);

    const ClockworkChannelMapState* m = clockwork_channel_map(c.core);
    REQUIRE(m != nullptr);
    clockwork_port_bus_detach_all(c.core);

    const uint32_t at = 5;   // above the two device inputs
    const ClockworkPort p = clockwork_port_open(clockwork_core_ports(c.core), "survivor", kClockworkPortSource, 2, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(c.core, p, at) == 1);
    REQUIRE(channelKind(m->in[at].load(std::memory_order_acquire)) == CLOCKWORK_CH_PORT);

    // A device switch: same engine, different widths.
    clockwork_init(c.core, kRate, kBlock, /*in*/ 4, /*out*/ 4, 0, arena2, sizeof arena2, nullptr, 0, 0);

    CHECK(m->device_in.load(std::memory_order_acquire)  == 4);
    // The device took channels 0-3 and said so...
    CHECK(channelKind(m->in[0].load(std::memory_order_acquire)) == CLOCKWORK_CH_DEVICE);
    // ...without trampling the stream that was already on 5.
    CHECK(channelKind(m->in[at].load(std::memory_order_acquire)) == CLOCKWORK_CH_PORT);

    clockwork_port_bus_detach_all(c.core);
    clockwork_port_close_all(clockwork_core_ports(c.core));
}
