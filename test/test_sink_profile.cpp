// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_sink_profile.cpp — the "<cell bytes>:<cells>,..." grammar that turns
 * memory_profile.h's strings into sink size classes, and the classes the
 * built profiles produce.
 */
#include "sink_profile.h"
#include "memory_profile.h"
#include "clockwork_event_sink.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <string>

namespace {
struct Parsed {
    uint32_t widths[CLOCKWORK_SINK_CLASSES_MAX] = {};
    uint32_t depths[CLOCKWORK_SINK_CLASSES_MAX] = {};
    uint32_t n = 0;
};
Parsed parse(const char* spec, uint32_t cap = CLOCKWORK_SINK_CLASSES_MAX) {
    Parsed p;
    p.n = clockwork_parse_sink_classes(spec, p.widths, p.depths, cap);
    return p;
}
}

TEST_CASE("sink profile: the grammar is width:depth pairs separated by commas",
          "[sinks][profile]") {
    const Parsed p = parse("1024:16,8192:2, 65536 : 1");
    REQUIRE(p.n == 3);
    CHECK(p.widths[0] == 1024); CHECK(p.depths[0] == 16);
    CHECK(p.widths[1] == 8192); CHECK(p.depths[1] == 2);
    CHECK(p.widths[2] == 65536); CHECK(p.depths[2] == 1);
}

TEST_CASE("sink profile: a malformed string yields nothing, never a partial shape",
          "[sinks][profile]") {
    CHECK(parse("").n == 0);
    CHECK(parse(nullptr).n == 0);
    CHECK(parse("1024").n == 0);
    CHECK(parse("1024:").n == 0);
    CHECK(parse(":16").n == 0);
    CHECK(parse("1024:16,").n == 0);
    CHECK(parse("1024:16,,8192:2").n == 0);
    CHECK(parse("1024x16").n == 0);
    CHECK(parse("1024:16 8192:2").n == 0);
    CHECK(parse("99999999999:1").n == 0);
    // Six classes into a cap of two: refused whole rather than truncated to
    // the first two, which would be a different shape than was asked for.
    CHECK(parse("1:1,2:1,3:1,4:1,5:1,6:1", 2).n == 0);
}

// What a string reserves once installed: every class's cells × width, each
// depth rounded up to a power of two of at least two, as the sink does.
static uint64_t reserves(const Parsed& p) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < p.n; ++i) {
        uint32_t d = std::max(p.depths[i], 2u);
        while (d & (d - 1)) ++d;
        total += uint64_t(d) * p.widths[i];
    }
    return total;
}

TEST_CASE("sink profile: the build's strings parse, and a sink reserves what they say",
          "[sinks][profile]") {
    const Parsed osc = parse(CLOCKWORK_OSC_SINK_CLASSES);
    REQUIRE(osc.n > 0);
    uint32_t widest = 0;
    for (uint32_t i = 0; i < osc.n; ++i) widest = std::max(widest, osc.widths[i]);
    // The documented desktop shape, when the build carries it unchanged.
    if (std::string(CLOCKWORK_OSC_SINK_CLASSES) == "1024:16,8192:2,16384:2,32768:2,65536:2") {
        CHECK(reserves(osc) == 256u * 1024u);   // a quarter of a megabyte per endpoint
        CHECK(widest == 65536u);                // a whole datagram
    }
    // Whatever the string is — the default, a CMake override, an embedded
    // profile — an OSC sink opened after the install the engine does at boot
    // is that shape. (This binary has no engine, so it installs here.)
    REQUIRE(clockwork_install_sink_profiles() != 0);
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:9", 0);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    CHECK(clockwork_sink_bytes_reserved(sink) == reserves(osc));
    CHECK(clockwork_sink_max_message_bytes(sink) == widest);
    clockwork_sink_close(sink);

    const Parsed midi = parse(CLOCKWORK_MIDI_SINK_CLASSES);
    REQUIRE(midi.n > 0);
    // The narrowest MIDI cell holds every channel and realtime message.
    uint32_t narrowest = ~0u;
    for (uint32_t i = 0; i < midi.n; ++i) narrowest = std::min(narrowest, midi.widths[i]);
    CHECK(narrowest >= 3);
}

TEST_CASE("sink profile: an installed shape is what a sink of that kind opens with",
          "[sinks][profile]") {
    // A shape of our own, then the header's back again so later cases see
    // the built-in one.
    const uint32_t widths[] = { 32, 4096 };
    const uint32_t depths[] = { 4, 1 };
    REQUIRE(clockwork_sink_profile(kClockworkSinkOsc, 2, widths, depths) != 0);
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:9", 0);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    CHECK(clockwork_sink_max_message_bytes(sink) == 4096u);
    // A class is two cells at least, so "4096:1" reserves two of them.
    CHECK(clockwork_sink_bytes_reserved(sink) == 4u * 32u + 2u * 4096u);
    clockwork_sink_close(sink);

    // A shape that does not hold together is refused, and the good one stays.
    const uint32_t twice[] = { 32, 32 };
    const uint32_t ones[]  = { 1, 1 };
    CHECK(clockwork_sink_profile(kClockworkSinkOsc, 2, twice, ones) == 0);
    const ClockworkSink again = clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:9", 0);
    REQUIRE(again != CLOCKWORK_SINK_NONE);
    CHECK(clockwork_sink_max_message_bytes(again) == 4096u);
    clockwork_sink_close(again);

    REQUIRE(clockwork_install_sink_profiles() != 0);
}
