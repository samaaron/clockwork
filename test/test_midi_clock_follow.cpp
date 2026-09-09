// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_midi_clock_follow.cpp — the MIDI clock-out follower verbs, through the
 * engine.
 *
 * /clockwork/midi/clock/follow, unfollow and followers go in at ingest() and
 * come out on the reply egress, and the pulses they start are recorded by
 * the NRT gateway's periodic tick and released by the render path the test
 * pumps — the whole path a client's clock takes, minus the port. What left
 * is read off the engine's own MidiClockOut: with no MIDI port open every
 * pulse is counted as having none to reach, which is the same number on a
 * box with a synth and on CI.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clock/MidiClockOut.h"
#include "clockwork_prefix.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <thread>

using engine_test::Engine;

namespace {

// Every byte that left the ring, delivered or counted as undeliverable.
uint64_t released(const MidiClockOut& m) { return m.sent() + m.dropped(); }

// Render `seconds` of audio, giving the gateway thread room to keep up: the
// freewheel clock advances a block per pump, and a follower whose grid ran
// more than a beat ahead of its last tick re-syncs instead of catching up,
// so a tight loop that starves the gateway would undercount by design.
void pumpGently(Engine& e, double seconds) {
    // The DSP's block, which is what one pump renders.
    const int blocks = static_cast<int>(seconds * engine_test::kSampleRate
                                        / get_audio_buffer_samples());
    for (int i = 0; i < blocks; ++i) {
        e.engine.pumpAudioBlock();
        if (i % 8 == 7) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

osc_test::Packet follow(const char* port, const char* timeline, int32_t token) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/clock/follow")) << port << timeline << token;
    return b.end();
}

osc_test::Packet unfollow(const char* port, int32_t token) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/clock/unfollow")) << port << token;
    return b.end();
}

osc_test::Packet followers(int32_t token) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/clock/followers")) << token;
    return b.end();
}

// The reply carrying this token, ignoring earlier replies to the same verb.
bool replyWithToken(Engine& e, const char* address, int32_t token, osc_test::ParsedReply& out) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(e.mu);
            for (const auto& r : e.replies) {
                if (osc_test::parseAddress(r.data(), static_cast<uint32_t>(r.size())) != address) continue;
                const auto p = osc_test::parseReply(r.data(), static_cast<uint32_t>(r.size()));
                if (p.argCount() > 0 && p.argInt(p.argCount() - 1) == token) { out = p; return true; }
            }
        }
        e.engine.pumpAudioBlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

TEST_CASE("midi/clock/follow clocks the Link timeline out until unfollow",
          "[engine][midi][clock]") {
    Engine e;
    MidiClockOut& out = e.engine.midiClockOut();

    // Follow every port on the Link timeline (the engine's default, 120 BPM).
    auto pkt = follow("*", "link", 7);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    osc_test::ParsedReply r;
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/follow.reply"), 7, r));
    CHECK(r.argCount() == 3);
    CHECK(r.argString(0) == "*");
    CHECK(r.argString(1) == "link");

    pkt = followers(8);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/followers.reply"), 8, r));
    REQUIRE(r.argCount() == 3);
    CHECK(r.argString(0) == "*");
    CHECK(r.argString(1) == "link");

    // A second of the engine's clock: 48 pulses at 120 BPM, give or take the
    // horizon the gateway records ahead and the block the render path
    // releases ahead.
    const uint64_t before = released(out);
    pumpGently(e, 1.0);
    const uint64_t during = released(out) - before;
    INFO(during << " bytes left in one second");
    CHECK(during >= 40);
    CHECK(during <= 56);

    // Unfollow: what is already recorded (at most the horizon) still goes
    // out, then nothing does.
    pkt = unfollow("*", 9);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/unfollow.reply"), 9, r));
    CHECK(r.argCount() == 2);
    CHECK(r.argString(0) == "*");
    pumpGently(e, 0.3);
    const uint64_t after = released(out);
    pumpGently(e, 1.0);
    CHECK(released(out) == after);

    pkt = followers(10);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/followers.reply"), 10, r));
    CHECK(r.argCount() == 1);   // the token alone: nobody follows
}

TEST_CASE("midi/clock/follow refuses a name that is nothing, and claims a midi:<port> nobody has clocked",
          "[engine][midi][clock]") {
    Engine e;
    osc_test::ParsedReply r;

    // Bare "midi" is the primary follower slot, and on a fresh engine with no
    // port claimed there is none: refused, not followed. The followers reply
    // that comes back after it proves the refusal was handled and changed
    // nothing.
    auto pkt = follow("other", "midi", 12);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    pkt = followers(13);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/followers.reply"), 13, r));
    CHECK(r.argCount() == 1);

    // A midi:<handle> that has never sent a pulse is a valid grid to follow
    // (it free-runs at its placeholder tempo until the port clocks), and the
    // follow claims its slot the way /clockwork/clock/midi:<handle>/tempo/set
    // would.
    pkt = follow("*", "midi:never-seen", 11);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/follow.reply"), 11, r));
    CHECK(r.argString(0) == "*");
    CHECK(r.argString(1) == "midi:never-seen");

    pkt = followers(14);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    REQUIRE(replyWithToken(e, CLOCKWORK_SYS("midi/clock/followers.reply"), 14, r));
    REQUIRE(r.argCount() == 3);
    CHECK(r.argString(0) == "*");
    CHECK(r.argString(1) == "midi:never-seen");
}
