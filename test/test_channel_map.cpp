// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_channel_map.cpp — every channel has an identity, always.
 *
 * The map exists because dsp_process is otherwise handed an anonymous range:
 * a stereo file's left and right are indistinguishable from each other and
 * from the microphone. So what is pinned here is that a channel always says
 * what it is — before a port, during one, and after it goes away — and that
 * the two halves keep their different promises:
 *
 *   HOT  (in[]/out[])  one word per channel, correct at the point of use.
 *   COLD (streams[])   name and range, guarded by `generation`.
 *
 * The ordering property matters more than any single value: the map describes
 * a channel no later than the audio it carries, which is why the entry is
 * written BEFORE the binding is published (audio_processor.cpp).
 */
#include "LanesFixture.h"

#include "lanes/lanes.h"
#include "shared_memory.h"
#include "clockwork_ports.h"
#include "clockwork_port_bus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

namespace {

// Everything a case binds goes away at its end, pass or fail — and the map is
// process-global, so a leftover binding would be another case's mystery.
struct Bus {
    Bus()  { clockwork_port_bus_detach_all(); }
    ~Bus() { clockwork_port_bus_detach_all(); clockwork_port_close_all(); }
};

const ClockworkChannelMapState* map() { return clockwork_channel_map(); }

uint32_t inWord (uint32_t c) { return map()->in [c].load(std::memory_order_acquire); }
uint32_t outWord(uint32_t c) { return map()->out[c].load(std::memory_order_acquire); }

// The entry a channel's word points at, or nullptr when it names no stream.
const ClockworkStreamEntry* streamOf(uint32_t word) {
    const uint32_t slot = channelSlot(word);
    if (slot == 0) return nullptr;
    return &map()->streams[slot - 1];
}

} // namespace

TEST_CASE("channel map: the device owns its own channels and nothing else does",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;
    REQUIRE(map() != nullptr);

    const uint32_t devIn  = map()->device_in.load(std::memory_order_acquire);
    const uint32_t devOut = map()->device_out.load(std::memory_order_acquire);
    REQUIRE(devIn  == lanes_test::kInChannels);
    REQUIRE(devOut == lanes_test::kOutChannels);

    for (uint32_t c = 0; c < devIn; ++c) {
        INFO("input channel " << c);
        REQUIRE(channelKind(inWord(c)) == CLOCKWORK_CH_DEVICE);
        REQUIRE(channelSlot(inWord(c)) == 0);      // the device is not a stream
    }
    for (uint32_t c = 0; c < devOut; ++c) {
        INFO("output channel " << c);
        REQUIRE(channelKind(outWord(c)) == CLOCKWORK_CH_DEVICE);
    }

    // Above the device, and with nothing bound, a channel says so rather than
    // being silently "ordinary".
    REQUIRE(channelKind(inWord(devIn))   == CLOCKWORK_CH_NONE);
    REQUIRE(channelKind(outWord(devOut)) == CLOCKWORK_CH_NONE);
}

TEST_CASE("channel map: a source claims input channels and names itself",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;
    const uint32_t first = lanes_test::kInChannels + 2;   // clear of the device

    const ClockworkPort p =
        clockwork_port_open("link:FakeLive/Main", kClockworkPortSource, 2, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, first) == 1);

    // HOT: by the time attach has returned, the channels already describe it.
    // The entry is written before the binding is published, so there is no
    // window in which the audio exists and the identity does not.
    for (uint32_t c = 0; c < 2; ++c) {
        INFO("channel " << (first + c));
        REQUIRE(channelKind(inWord(first + c)) == CLOCKWORK_CH_PORT);
        REQUIRE(channelSlot(inWord(first + c)) != 0);
    }
    // Both channels of one port name the SAME stream.
    REQUIRE(channelSlot(inWord(first)) == channelSlot(inWord(first + 1)));

    // COLD: and the stream says what it is.
    const ClockworkStreamEntry* e = streamOf(inWord(first));
    REQUIRE(e != nullptr);
    CHECK(e->port      == p);
    CHECK(e->direction == static_cast<uint32_t>(kClockworkPortSource));
    CHECK(e->first     == first);
    CHECK(e->count     == 2);
    CHECK(std::string(e->name) == "link:FakeLive/Main");

    // A source occupies INPUT channels only — the same side pull_port_sources
    // reads. Its number on the output side is untouched.
    CHECK(channelKind(outWord(first)) != CLOCKWORK_CH_PORT);
}

TEST_CASE("channel map: a sink claims output channels, not input",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;
    const uint32_t first = lanes_test::kOutChannels + 2;

    const ClockworkPort p = clockwork_port_open("disk-out", kClockworkPortSink, 2, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, first) == 1);

    REQUIRE(channelKind(outWord(first))     == CLOCKWORK_CH_PORT);
    REQUIRE(channelKind(outWord(first + 1)) == CLOCKWORK_CH_PORT);
    CHECK(channelKind(inWord(first))        != CLOCKWORK_CH_PORT);

    const ClockworkStreamEntry* e = streamOf(outWord(first));
    REQUIRE(e != nullptr);
    CHECK(e->direction == static_cast<uint32_t>(kClockworkPortSink));
    CHECK(std::string(e->name) == "disk-out");
}

TEST_CASE("channel map: detaching gives the channels back to what they were",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;

    // One above the device, one ON it — the two cases have different answers.
    const uint32_t above = lanes_test::kInChannels + 4;
    const ClockworkPort hi = clockwork_port_open("above", kClockworkPortSource, 2, 1024);
    const ClockworkPort lo = clockwork_port_open("onto-device", kClockworkPortSource, 1, 1024);
    REQUIRE(clockwork_port_bus_attach(hi, above) == 1);
    REQUIRE(clockwork_port_bus_attach(lo, 0)     == 1);

    REQUIRE(channelKind(inWord(above)) == CLOCKWORK_CH_PORT);
    REQUIRE(channelKind(inWord(0))     == CLOCKWORK_CH_PORT);   // overrides the device

    clockwork_port_bus_detach(hi);
    clockwork_port_bus_detach(lo);

    // Above the device: nothing there any more, and it says so.
    CHECK(channelKind(inWord(above)) == CLOCKWORK_CH_NONE);
    CHECK(channelSlot(inWord(above)) == 0);
    // On the device: the device has it back, not "nothing".
    CHECK(channelKind(inWord(0))     == CLOCKWORK_CH_DEVICE);

    // And the stream slot is free rather than stale.
    for (uint32_t i = 0; i < CHANNEL_MAP_MAX_STREAMS; ++i)
        CHECK(map()->streams[i].port != hi);
}

TEST_CASE("channel map: two streams get distinct slots and do not collide",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;
    const uint32_t a = lanes_test::kInChannels + 2;
    const uint32_t b = lanes_test::kInChannels + 8;

    const ClockworkPort pa = clockwork_port_open("peer-a", kClockworkPortSource, 2, 1024);
    const ClockworkPort pb = clockwork_port_open("peer-b", kClockworkPortSource, 2, 1024);
    REQUIRE(clockwork_port_bus_attach(pa, a) == 1);
    REQUIRE(clockwork_port_bus_attach(pb, b) == 1);

    REQUIRE(channelSlot(inWord(a)) != channelSlot(inWord(b)));
    CHECK(std::string(streamOf(inWord(a))->name) == "peer-a");
    CHECK(std::string(streamOf(inWord(b))->name) == "peer-b");

    // Detaching one leaves the other exactly where it was — the failure this
    // guards is a slot being reused or cleared by its neighbour.
    clockwork_port_bus_detach(pa);
    CHECK(channelKind(inWord(a)) == CLOCKWORK_CH_NONE);
    REQUIRE(channelKind(inWord(b)) == CLOCKWORK_CH_PORT);
    CHECK(std::string(streamOf(inWord(b))->name) == "peer-b");
}

TEST_CASE("channel map: the generation is even at rest and moves across a change",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;

    const uint32_t before = map()->generation.load(std::memory_order_acquire);
    CHECK((before % 2) == 0);   // even: nobody is mid-write

    const ClockworkPort p = clockwork_port_open("gen-check", kClockworkPortSource, 1, 1024);
    REQUIRE(clockwork_port_bus_attach(p, lanes_test::kInChannels + 1) == 1);

    const uint32_t after = map()->generation.load(std::memory_order_acquire);
    CHECK(after != before);
    CHECK((after % 2) == 0);    // and back to even when the write finished
}

TEST_CASE("channel map: a long name is truncated, not overrun",
          "[channel-map]") {
    lanes_test::boot();
    Bus guard;
    const std::string longName(CHANNEL_MAP_NAME_BYTES * 2, 'x');

    const ClockworkPort p =
        clockwork_port_open(longName.c_str(), kClockworkPortSource, 1, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, lanes_test::kInChannels + 1) == 1);

    const ClockworkStreamEntry* e = streamOf(inWord(lanes_test::kInChannels + 1));
    REQUIRE(e != nullptr);
    CHECK(std::strlen(e->name) == CHANNEL_MAP_NAME_BYTES - 1);   // NUL-terminated
    CHECK(e->name[CHANNEL_MAP_NAME_BYTES - 1] == '\0');
}

TEST_CASE("channel map: detach_all leaves nothing claimed", "[channel-map]") {
    lanes_test::boot();
    Bus guard;
    const ClockworkPort p = clockwork_port_open("sweep-me", kClockworkPortSource, 2, 1024);
    REQUIRE(clockwork_port_bus_attach(p, lanes_test::kInChannels + 2) == 1);

    clockwork_port_bus_detach_all();

    for (uint32_t c = 0; c < CLOCKWORK_MAX_CHANNELS; ++c) {
        INFO("channel " << c);
        REQUIRE(channelKind(inWord(c))  != CLOCKWORK_CH_PORT);
        REQUIRE(channelKind(outWord(c)) != CLOCKWORK_CH_PORT);
    }
    for (uint32_t i = 0; i < CHANNEL_MAP_MAX_STREAMS; ++i)
        REQUIRE(map()->streams[i].port == 0);
}

TEST_CASE("channel map: the arena and the accessor are the same bytes",
          "[channel-map]") {
    lanes_test::boot();
    // What a client finds by offset is what the guest is handed by pointer.
    const auto* byOffset = reinterpret_cast<const ClockworkChannelMapState*>(
        static_cast<const uint8_t*>(clockwork_lanes_base()) + CHANNEL_MAP_START);
    REQUIRE(map() == byOffset);
}
