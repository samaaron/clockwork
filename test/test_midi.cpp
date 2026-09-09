// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_midi.cpp — the /midi/ subsystem routed through the engine ingress
 * (sendOSC -> ingest -> MidiControl -> Rust clockwork_midi subsystem -> egress).
 *
 * No MIDI hardware required: these pin that /midi commands reach the subsystem
 * and that its replies/pushes come back through the egress, that subscription
 * pushes a ports snapshot, and that the out/clock dispatch paths are robust with
 * no devices open (port lists are typically empty on a headless CI box). Actual
 * device loopback is covered by the Rust virtual-port test (run manually).
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "clockwork_prefix.h"
#include "OscTestUtils.h"
#include "clock/clock_math.h"   // wallClockNTP

#ifdef CLOCKWORK_MIDI

// /schedule expects an NTP timetag in the engine's clock domain. The engine
// compares OSC timetags as int64, into which a present-day NTP value overflows
// — consistently, so ordering holds, but a small literal like 1.0 is NOT "the
// distant past": it compares as the far future and the event never fires.
// Always timetag relative to wall-clock now (wallClockNTP from WallClock.h).

TEST_CASE("/clockwork/midi/ports/list replies through the engine", "[midi]") {
    EngineFixture fx;
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/midi/ports/list"));

    OscReply r;
    REQUIRE(fx.waitForReply("/clockwork/midi/ports.reply", r));
    // First arg is the input-port count: ≥ 0 even with no devices attached.
    CHECK(r.parsed().argInt(0) >= 0);
}

TEST_CASE("/clockwork/midi/notify/subscribe pushes a ports snapshot", "[midi]") {
    EngineFixture fx;
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/midi/notify/subscribe"));

    OscReply r;
    CHECK(fx.waitForReply("/clockwork/midi/ports.reply", r));
}

TEST_CASE("an inbound MIDI event reaches the subscriber, whichever side produced it", "[midi]") {
    // A subsystem's callback writes its events into the IN ring; a host's
    // front on the web does the same. From there one audio-thread route
    // sends them to the MIDI audience — so an event pushed in here looks, to
    // a subscriber, exactly like one a port sent. The fixture's transport
    // surfaces every broadcast, so the subscription is for the audience's
    // sake and the assertion is on delivery.
    EngineFixture fx;
    fx.send(osc_test::message("/clockwork/midi/notify/subscribe"));
    fx.clearReplies();

    osc_test::Builder b;
    b.begin("/clockwork/midi/in/note_on")
        << "keys" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100) << osc::TimeTag(0xdeadbeef00000001ull);
    fx.send(b.end());

    OscReply r;
    REQUIRE(fx.waitForReply("/clockwork/midi/in/note_on", r));
    const auto p = r.parsed();
    CHECK(p.argString(0) == "keys");
    CHECK(p.argInt(2) == 60);
    CHECK(p.argCount() == 5);   // the arrival timetag rode along

    // The verb that lives under the same name is still a verb: it opens a
    // port (none by that name here) and is never broadcast back as an event.
    fx.clearReplies();
    osc_test::Builder en;
    en.begin("/clockwork/midi/in/enable") << "no_such_port" << static_cast<osc::int32>(1);
    fx.send(en.end());
    CHECK_FALSE(fx.waitForReply("/clockwork/midi/in/enable", r, 300));
    CHECK_FALSE(fx.waitForReply("/clockwork/error", r, 100));
}

TEST_CASE("/clockwork/midi/refresh broadcasts a ports update", "[midi]") {
    EngineFixture fx;
    // A subscriber is needed for the broadcast to have an in-process audience.
    fx.send(osc_test::message("/clockwork/midi/notify/subscribe"));
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/midi/refresh"));

    OscReply r;
    CHECK(fx.waitForReply("/clockwork/midi/ports", r));
}

// ── The client half: midi/out/* and midi/clock/beat ──────────────────────────
// These verbs are CLOCKWORK_CLIENT_VERBS (docs/SURFACE.md); a build without
// them refuses them by name, and test_client_verbs_absent.cpp pins that.
#if CLOCKWORK_CLIENT_VERBS

TEST_CASE("/midi out + clock dispatch is robust with no devices open", "[midi]") {
    EngineFixture fx;

    // None of these have an open destination, so they are no-ops — but must not
    // crash the subsystem or the engine.
    osc_test::Builder noteOn;
    noteOn.begin("/clockwork/midi/out/note_on")
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100);
    fx.send(noteOn.end());

    osc_test::Builder beat;
    beat.begin("/clockwork/midi/clock/beat") << "out" << 500.0f;
    fx.send(beat.end());

    osc_test::Builder sync;
    sync.begin("/clockwork/midi/clocktempo/get") << "in" << static_cast<osc::int32>(1);
    fx.send(sync.end());

    // The engine is still alive and serving /midi afterwards.
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/clockwork/midi/ports.reply", r));
}

TEST_CASE("/clockwork/midi/at schedules a wrapped event without crashing", "[midi]") {
    EngineFixture fx;

    // Inner event the scheduler will dispatch when due.
    osc_test::Builder inner;
    inner.begin("/clockwork/midi/out/note_on")
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(60)
        << static_cast<osc::int32>(100);
    osc_test::Packet innerPkt = inner.end();

    // Wrap as /clockwork/schedule <d: ntpSeconds (just past → due immediately)> <b: inner OSC>.
    // The inner /midi/out is self-routing: on fire it re-ingests through the same
    // dispatch an immediate /midi/out hits.
    osc_test::Builder at;
    at.begin(CLOCKWORK_SYS("schedule"))
        << (wallClockNTP() - 1.0)
        << osc::Blob(innerPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(innerPkt.size()));
    fx.send(at.end());

    // Let the audio thread drain the scheduler and the dispatch thread run.
    fx.waitForBlocks(5);

    // Engine is still alive and serving /midi.
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/clockwork/midi/ports.reply", r));
}

TEST_CASE("/schedule-wrapped /midi/clock/beat reaches the engine clock-out", "[midi][midi_clock]") {
    // The beat verb's real wire form: a client wraps it in a timetagged
    // /schedule rather than sending it immediately, so it must survive the
    // schedule→fire→dispatch round trip and reach MidiClockOut
    // via the unified dispatch (same path an immediate /midi/clock/beat would hit).
    // The precise per-beat tick count is covered by test_midi_clock_out.cpp, which
    // drives MidiClockOut directly; fired output now flows through the private
    // control ring (not the outbound ring), so it isn't tapped here — this asserts
    // the integration path is live and crash-free.
    EngineFixture fx;

    osc_test::Builder inner;
    inner.begin("/clockwork/midi/clock/beat") << "clk" << 100.0f;   // 24 ticks over 100 ms
    osc_test::Packet innerPkt = inner.end();

    osc_test::Builder at;
    at.begin(CLOCKWORK_SYS("schedule"))
        << (wallClockNTP() - 1.0)                               // just past → due immediately
        << osc::Blob(innerPkt.data.data(),
                     static_cast<osc::osc_bundle_element_size_t>(innerPkt.size()));
    fx.send(at.end());

    fx.waitForBlocks(10);   // beat fires → dispatch → MidiClockOut generates pulses

    // Engine is still alive and serving /midi after the deferred clock-out path ran.
    fx.clearReplies();
    fx.send(osc_test::message("/clockwork/midi/ports/list"));
    OscReply r;
    CHECK(fx.waitForReply("/clockwork/midi/ports.reply", r));
}

#endif // CLOCKWORK_CLIENT_VERBS
#endif // CLOCKWORK_MIDI
