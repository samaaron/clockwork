// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_midi_clock_out.cpp — 24 pulses a beat, and they are ClockworkClock's.
 *
 * These run in EVERY build that compiles MIDI in, with clockwork's timed
 * queue on or off, and that is the point of them: a guest that holds its own
 * schedule builds without the queue, and MIDI clock out must still work
 * there. Compiling these only alongside the queue would leave that build
 * untested. A suite that
 * only ran in one of the two configurations was exactly how that survived.
 *
 * DEVICE-INDEPENDENT BY CONSTRUCTION. Most of what matters here is *when* a
 * tick is released, not what a port did with it, so the assertions are on
 * `sent + dropped` — every tick that left the ring, whether a port took it or
 * there was no port to take it. That number is the same on a box with MIDI and
 * on one without, so these cases do not skip on CI, in a container, or here.
 * The cases that genuinely need a port say so and are guarded.
 */
#include "clock/MidiClockOut.h"
#include "clock/ClockworkClock.h"
#include "clockwork_event_sink.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

namespace {

// Every tick that has left the ring: delivered to a port, or counted as having
// no port to go to. Never silently vanished — which is a property this file
// asserts rather than assumes (see "a burst with no port open" below).
uint64_t released(const MidiClockOut& m) { return m.sent() + m.dropped(); }

// A beat one tick per second wide. Ticks land 1 s apart, so the microseconds
// between capturing t0 and onBeat() reading the clock cannot move a tick
// across a horizon and make these assertions flaky.
constexpr double kWideBeat = 24.0;

} // namespace

TEST_CASE("midi clock: a beat is 24 pulses spread over its duration",
          "[midi][clock]") {
    ClockworkClock clock;
    MidiClockOut m;

    const double t0 = clock.now();
    m.onBeat(clock, "*", kWideBeat);
    const uint64_t base = released(m);

    // Nothing is due yet: the first pulse is at t0, and generate() only
    // releases what falls inside the look-ahead window ahead of `nowNtp`.
    m.generate(t0 - 1.0);
    CHECK(released(m) - base == 0);

    // Half a second in, only the pulse at t0 has come due.
    m.generate(t0 + 0.5);
    CHECK(released(m) - base == 1);

    // Five and a half seconds in: the pulses at t0+0 .. t0+5.
    m.generate(t0 + 5.5);
    CHECK(released(m) - base == 6);

    // Well past the end — all 24, and not one more.
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == MidiClockOut::kPulsesPerBeat);

    // Idempotent: the ring is empty and a further call releases nothing.
    m.generate(t0 + 200.0);
    CHECK(released(m) - base == MidiClockOut::kPulsesPerBeat);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: a tick is never released before its time",
          "[midi][clock]") {
    ClockworkClock clock;
    MidiClockOut m;

    const double t0 = clock.now();
    m.onBeat(clock, "*", kWideBeat);
    const uint64_t base = released(m);

    // Walk the horizon forward one second at a time. Releasing early would be
    // a tick on the wire ahead of the beat it belongs to, which is the one
    // failure a MIDI clock must not have.
    for (int i = 0; i < static_cast<int>(MidiClockOut::kPulsesPerBeat); ++i) {
        m.generate(t0 + static_cast<double>(i) + 0.5);
        INFO("horizon at t0 + " << i << ".5s");
        REQUIRE(released(m) - base == static_cast<uint64_t>(i + 1));
    }

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: the ring is finite and a full one is counted",
          "[midi][clock]") {
    ClockworkClock clock;
    MidiClockOut m;

    const uint64_t base = m.dropped();

    // The ring holds kSlots - 1 ticks: one slot is always left empty so a full
    // ring is distinguishable from an empty one. 11 bursts is 264 ticks into
    // 255 places, so exactly 9 must be refused — and refused rather than
    // overwriting a tick already recorded.
    constexpr int kBursts = 11;
    for (int i = 0; i < kBursts; ++i) m.onBeat(clock, "*", kWideBeat);

    const uint64_t offered  = kBursts * MidiClockOut::kPulsesPerBeat;  // 264
    const uint64_t capacity = MidiClockOut::kSlots - 1;                // 255
    CHECK(m.dropped() - base == offered - capacity);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: reset abandons everything pending", "[midi][clock]") {
    ClockworkClock clock;
    MidiClockOut m;

    const double t0 = clock.now();
    m.onBeat(clock, "*", kWideBeat);
    const uint64_t base = released(m);

    m.reset();
    // The reset is consumed here and the ring emptied; nothing is released by
    // this call even though every tick is long overdue.
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == 0);

    // And the abandonment is permanent — they do not reappear next block.
    m.generate(t0 + 200.0);
    CHECK(released(m) - base == 0);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: every generated tick is accounted for", "[midi][clock]") {
    ClockworkClock clock;
    MidiClockOut m;

    // Are there MIDI ports on this machine? Asked, not assumed: this case has
    // to be true on a CI container with no sound card AND on a laptop with a
    // synth plugged in, so it asserts the invariant that holds either way and
    // only then narrows.
    ClockworkSink open[64];
    const uint32_t n = clockwork_sink_list(open, 64);
    uint32_t midiSinks = 0;
    for (uint32_t i = 0; i < n && i < 64; ++i)
        if (clockwork_sink_kind(open[i]) == kClockworkSinkMidi) ++midiSinks;

    const double t0 = clock.now();
    m.onBeat(clock, "*", kWideBeat);
    const uint64_t baseSent = m.sent(), baseDropped = m.dropped();

    m.generate(t0 + 100.0);

    const uint64_t sent = m.sent() - baseSent;
    const uint64_t dropped = m.dropped() - baseDropped;

    // THE INVARIANT: nothing vanishes. Every pulse either reached a port or was
    // counted as having none to reach. True on every platform, which is what
    // makes this runnable in CI rather than only where MIDI happens to work.
    INFO(midiSinks << " MIDI sink(s) open; " << sent << " sent, " << dropped << " dropped");
    CHECK(sent + dropped >= static_cast<uint64_t>(MidiClockOut::kPulsesPerBeat));

    if (midiSinks == 0) {
        // Narrowed: with no port at all, all 24 are counted as undelivered. A
        // session with nothing plugged in otherwise produces 24 ticks a beat
        // with every counter at zero, which reads exactly like working.
        CHECK(sent == 0);
        CHECK(dropped == static_cast<uint64_t>(MidiClockOut::kPulsesPerBeat));
    }

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: an unnamed port fans out, a named one does not",
          "[midi][clock]") {
    // Resolution is a command-thread decision and is observable without a
    // device: "*" and "" mean every port and cannot be one sink.
    CHECK(midi_clock_sink_for_port("*") == CLOCKWORK_SINK_NONE);
    CHECK(midi_clock_sink_for_port("") == CLOCKWORK_SINK_NONE);
}

// ── Following a timeline ─────────────────────────────────────────────────────
//
// A follower is the other producer: not a burst a client asked for, but a
// continuous 24-PPQN clock on a timeline's own grid, generated by the engine
// from a periodic tick(). The clock here is a bare ClockworkClock whose Link
// timeline is set by hand, and `now` is handed to tick() by the test, so
// every pulse time below is arithmetic on known numbers and nothing sleeps.

namespace {

// 24 PPQN at 120 BPM: a pulse every 1/48 s.
constexpr double kPulse120 = 1.0 / 48.0;

// A clock whose Link timeline has beat 0 at t0, at 120 BPM and stopped.
void gridAt120(ClockworkClock& clock, double t0) {
    clock.state()->setTempo(120.0, t0);
    clock.state()->setTransport(false, 0.0);
}

// How many pulses of that grid fall in [t0, upTo]: pulse k is at t0 + k/48,
// and a follower whose horizon reaches upTo has committed exactly these.
// (No test value below puts upTo on a pulse, so rounding cannot move one.)
uint64_t gridPulsesUpTo(double t0, double upTo) {
    return static_cast<uint64_t>(std::floor((upTo - t0) / kPulse120)) + 1;
}

} // namespace

TEST_CASE("midi clock: a follower puts a pulse on every 1/24 beat of its timeline",
          "[midi][clock][follow]") {
    ClockworkClock clock;
    const double t0 = 1000.0;
    gridAt120(clock, t0);

    MidiClockOut m;
    REQUIRE(m.follow("*", "link", 0, CLOCKWORK_SINK_NONE));
    m.tick(clock, t0);
    const uint64_t base = released(m);

    // Nothing before the grid.
    m.generate(t0 - 1.0);
    CHECK(released(m) - base == 0);

    // Pulse k lands at t0 + k/48: walk the horizon so that each call admits
    // exactly one more (generate releases what is within kLookaheadSeconds).
    for (int k = 0; k < 5; ++k) {
        m.generate(t0 + k * kPulse120 + 0.005 - MidiClockOut::kLookaheadSeconds);
        INFO("pulse " << k);
        REQUIRE(released(m) - base == static_cast<uint64_t>(k + 1));
    }

    // One tick commits exactly the pulses inside its horizon, no more.
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == gridPulsesUpTo(t0, t0 + MidiClockOut::kFollowHorizonSeconds));

    // The next tick continues from the pulse after the last one: no gap, no
    // repeat, whatever the tick cadence.
    m.tick(clock, t0 + 0.03);
    m.tick(clock, t0 + 0.11);
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == gridPulsesUpTo(t0, t0 + 0.11 + MidiClockOut::kFollowHorizonSeconds));

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: a tempo change re-pins the pulses not yet committed",
          "[midi][clock][follow]") {
    ClockworkClock clock;
    const double t0 = 1000.0;
    gridAt120(clock, t0);

    MidiClockOut m;
    REQUIRE(m.follow("*", "link", 0, CLOCKWORK_SINK_NONE));
    m.tick(clock, t0);
    const uint64_t committed = gridPulsesUpTo(t0, t0 + MidiClockOut::kFollowHorizonSeconds);
    m.generate(t0 + 100.0);
    const uint64_t base = released(m);
    REQUIRE(base == committed);

    // Halve the tempo at t1 with the beat continuous there (what tempo/set
    // does). The follower has sent pulses 0..committed-1; the next one is
    // still beat committed/24, now on the 60 BPM grid.
    const double t1 = t0 + MidiClockOut::kFollowHorizonSeconds;
    clock.state()->retempo(60.0, t1);
    const clockwork::Timeline slow = clock.timeline(0);
    const double next = slow.timeAtBeat(static_cast<double>(committed) / 24.0);
    CHECK(next > t1);

    m.tick(clock, t1);
    m.generate(next - 0.001 - MidiClockOut::kLookaheadSeconds);
    CHECK(released(m) - base == 0);
    m.generate(next + 0.001 - MidiClockOut::kLookaheadSeconds);
    CHECK(released(m) - base == 1);

    // And the one after it is 1/24 s later — the new grid's spacing.
    const double after = next + 1.0 / 24.0;
    m.generate(after - 0.001 - MidiClockOut::kLookaheadSeconds);
    CHECK(released(m) - base == 1);
    m.generate(after + 0.001 - MidiClockOut::kLookaheadSeconds);
    CHECK(released(m) - base == 2);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: the clock runs while the transport is stopped; Start and Stop mark the transitions",
          "[midi][clock][follow]") {
    ClockworkClock clock;
    const double t0 = 1000.0;
    gridAt120(clock, t0);   // stopped

    MidiClockOut m;
    REQUIRE(m.follow("*", "link", 0, CLOCKWORK_SINK_NONE));
    m.tick(clock, t0);
    m.generate(t0 + 100.0);
    const uint64_t firstTick = released(m);
    // A stopped transport still clocks, and adopting its state sends nothing.
    CHECK(firstTick == gridPulsesUpTo(t0, t0 + MidiClockOut::kFollowHorizonSeconds));

    // Playing from t1: one Start, at t1, ahead of the pulses that follow it.
    const double t1 = t0 + MidiClockOut::kFollowHorizonSeconds;
    clock.state()->setTransport(true, t1);
    m.tick(clock, t1);
    m.generate(t1 - 0.005 - MidiClockOut::kLookaheadSeconds);
    CHECK(released(m) - firstTick == 0);
    m.generate(t1 + 0.001 - MidiClockOut::kLookaheadSeconds);
    CHECK(released(m) - firstTick == 1);                     // the Start alone
    m.generate(t1 + 100.0);
    CHECK(released(m) == gridPulsesUpTo(t0, t1 + MidiClockOut::kFollowHorizonSeconds) + 1);

    // Stopped again: one Stop. Unchanged transport: nothing extra.
    const double t2 = t1 + MidiClockOut::kFollowHorizonSeconds;
    clock.state()->setTransport(false, t2);
    m.tick(clock, t2);
    m.tick(clock, t2 + 0.001);
    m.generate(t2 + 100.0);
    CHECK(released(m) == gridPulsesUpTo(t0, t2 + 0.001 + MidiClockOut::kFollowHorizonSeconds) + 2);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: unfollow stops the pulses and reset forgets every follower",
          "[midi][clock][follow]") {
    ClockworkClock clock;
    const double t0 = 1000.0;
    gridAt120(clock, t0);

    MidiClockOut m;
    REQUIRE(m.follow("*", "link", 0, CLOCKWORK_SINK_NONE));
    m.tick(clock, t0);
    m.generate(t0 + 100.0);
    const uint64_t base = released(m);
    REQUIRE(base > 0);

    // Unfollowing a port nobody follows is not an error worth a reply.
    CHECK(!m.unfollow("nothing"));
    CHECK(m.unfollow("*"));
    CHECK(m.followers().empty());
    m.tick(clock, t0 + 1.0);
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == 0);

    // reset() is what the engine calls at shutdown: it clears the ring AND
    // the follower table, so nothing resumes on the next boot.
    REQUIRE(m.follow("*", "link", 0, CLOCKWORK_SINK_NONE));
    m.tick(clock, t0 + 2.0);
    m.reset();
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == 0);
    CHECK(m.followers().empty());
    m.tick(clock, t0 + 3.0);
    m.generate(t0 + 100.0);
    CHECK(released(m) - base == 0);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: the follower table is per port, re-targetable and finite",
          "[midi][clock][follow]") {
    ClockworkClock clock;
    MidiClockOut m;

    REQUIRE(m.follow("a", "link", 0, CLOCKWORK_SINK_NONE));
    REQUIRE(m.follow("b", "midi:iac", 1, CLOCKWORK_SINK_NONE));
    auto list = m.followers();
    REQUIRE(list.size() == 2);
    CHECK(list[0].port == "a");
    CHECK(list[0].timeline == "link");
    CHECK(list[1].port == "b");
    CHECK(list[1].timeline == "midi:iac");

    // Following a port again re-targets it rather than adding a second entry.
    REQUIRE(m.follow("a", "midi", 2, CLOCKWORK_SINK_NONE));
    list = m.followers();
    REQUIRE(list.size() == 2);
    CHECK(list[0].port == "a");
    CHECK(list[0].timeline == "midi");

    // Finite: the table holds kMaxFollowers, and one more is refused, not
    // silently dropped or grown on the command thread.
    for (uint32_t i = list.size(); i < MidiClockOut::kMaxFollowers; ++i)
        REQUIRE(m.follow("port-" + std::to_string(i), "link", 0, CLOCKWORK_SINK_NONE));
    CHECK(!m.follow("one-too-many", "link", 0, CLOCKWORK_SINK_NONE));
    CHECK(m.followers().size() == MidiClockOut::kMaxFollowers);

    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: two followers on one ring stay in time order",
          "[midi][clock][follow]") {
    // The render thread releases the ring front to back and stops at the
    // first slot beyond its horizon, so a slot out of time order would hold
    // everything behind it. Two followers on two grids — Link's at t0, and a
    // midi slot anchored wherever the wall clock stood when it was claimed —
    // interleave by time.
    ClockworkClock clock;
    const double t0 = 1000.0;
    gridAt120(clock, t0);
    // midi:x claimed by hand: beat 0 at t0 + a quarter pulse, at 120.
    const int mid = clock.resolveOrClaimTimeline("midi:x");
    REQUIRE(mid > 0);
    clock.setMidiTimelineTempo(mid, 120.0);

    MidiClockOut m;
    REQUIRE(m.follow("a", "link", 0, CLOCKWORK_SINK_NONE));
    REQUIRE(m.follow("b", "midi:x", mid, CLOCKWORK_SINK_NONE));
    m.tick(clock, t0);
    const uint64_t base = released(m);

    // Every generate() call from here on releases everything due so far: if
    // the ring were out of order the count would lag the time.
    uint64_t last = 0;
    for (double h = t0 - MidiClockOut::kLookaheadSeconds; h < t0 + MidiClockOut::kFollowHorizonSeconds; h += 0.002) {
        m.generate(h);
        const double due = h + MidiClockOut::kLookaheadSeconds;
        const clockwork::Timeline a = clock.timeline(0), b = clock.timeline(mid);
        uint64_t expect = 0;
        for (int64_t k = 0; ; ++k) {
            const double at = a.timeAtBeat(k / 24.0);
            if (at > t0 + MidiClockOut::kFollowHorizonSeconds) break;
            if (at <= due) ++expect;
        }
        const int64_t bFirst = static_cast<int64_t>(std::ceil(b.beatAt(t0) * 24.0));
        for (int64_t k = bFirst; ; ++k) {
            const double at = b.timeAtBeat(k / 24.0);
            if (at > t0 + MidiClockOut::kFollowHorizonSeconds) break;
            if (at <= due) ++expect;
        }
        INFO("horizon " << h - t0);
        REQUIRE(released(m) - base == expect);
        REQUIRE(released(m) - base >= last);
        last = released(m) - base;
    }

    clock.stopBackgroundWork();
}

// ── Cases that genuinely need a MIDI port ────────────────────────────────────
//
// Everything above runs everywhere. These need a port to open, which needs
// access to the platform's MIDI service — on Linux that is /dev/snd/seq, and a
// container or a user outside the `audio` group does not have it. Guarded at
// run time rather than compiled out, so they start passing the moment a box
// has MIDI instead of needing anyone to remember to switch them on.

TEST_CASE("midi clock: ticks reach an open port, carrying their own time",
          "[midi][clock][device]") {
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkMidi, "clockwork-clock-test", 256);
    if (sink == CLOCKWORK_SINK_NONE) {
        WARN("no MIDI port could be opened — skipping delivery. On Linux: "
             "usermod -aG audio $USER, then log in again.");
        return;
    }

    ClockworkClock clock;
    MidiClockOut m;

    ClockworkSinkStats before{};
    REQUIRE(clockwork_sink_stats(sink, &before));

    const double t0 = clock.now();
    m.onBeat(clock, "clockwork-clock-test", kWideBeat);
    m.generate(t0 + 100.0);

    ClockworkSinkStats after{};
    REQUIRE(clockwork_sink_stats(sink, &after));

    // Every pulse reached the sink, and the sink took them all.
    CHECK(m.sent() >= static_cast<uint64_t>(MidiClockOut::kPulsesPerBeat));
    CHECK(after.dropped == before.dropped);

    clockwork_sink_close(sink);
    clock.stopBackgroundWork();
}

TEST_CASE("midi clock: naming the same port twice reuses one sink",
          "[midi][clock][device]") {
    const ClockworkSink a = midi_clock_sink_for_port("clockwork-dedupe-test");
    if (a == CLOCKWORK_SINK_NONE) {
        WARN("no MIDI port could be opened — skipping sink reuse.");
        return;
    }
    // A second sink on the same port would double every tick on the wire.
    const ClockworkSink b = midi_clock_sink_for_port("clockwork-dedupe-test");
    CHECK(a == b);
    clockwork_sink_close(a);
}

// ── End to end: a tick MidiClockOut generates reaches a real device ──────────
//
// Everything above proves MidiClockOut fills its ring correctly and hands ticks
// to a sink. tests/midi_endpoint.rs (Rust) proves a sink reaches a real port in
// time order. NEITHER proves the join: a tick could be handed to a sink handle
// that resolves to nothing and every counter here would still read correctly,
// because the ticks WERE handed over. That is exactly the shape of the bug this
// file was written after.
//
// So this drives the whole path — MidiClockOut → clockwork_sink_send → the drain →
// MidiIo → ALSA → the kernel — and then reads the pulses back. `Midi Through`
// is a kernel loopback port present on essentially every Linux system: what we
// send to its output arrives at its input, where the MIDI subsystem decodes
// 0xF8 and calls the clock callback. Counting those callbacks counts pulses
// that genuinely went out and came back.

#include "clockwork_midi.h"
#include "clockwork_prefix.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <cctype>
#include <cstring>
#include <thread>
#include <chrono>

namespace {

struct MidiCtx {
    std::vector<std::string> outPorts, inPorts;
    std::atomic<int>         pulses{0};
};

void emit_cb(void* ctx, int32_t, const uint8_t* osc, uint32_t len) {
    auto* c = static_cast<MidiCtx*>(ctx);
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(osc),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        if (std::strcmp(msg.AddressPattern(), CLOCKWORK_SYS("midi/ports.reply")) != 0) return;
        // <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*
        auto it = msg.ArgumentsBegin();
        const int nIn = (it++)->AsInt32();
        c->inPorts.clear();
        for (int i = 0; i < nIn; ++i) {
            c->inPorts.emplace_back((it++)->AsString());
            ++it;  // enabled
        }
        const int nOut = (it++)->AsInt32();
        c->outPorts.clear();
        for (int i = 0; i < nOut; ++i) {
            c->outPorts.emplace_back((it++)->AsString());
            ++it;
        }
    } catch (...) { /* not the reply we wanted */ }
}

void clock_cb(void* ctx, const uint8_t*, uint32_t, const uint8_t*, uint32_t, uint64_t) {
    static_cast<MidiCtx*>(ctx)->pulses.fetch_add(1, std::memory_order_relaxed);
}
void transport_cb(void*, const uint8_t*, uint32_t, const uint8_t*, uint32_t, int32_t, double) {}

// The normalised handle for the first port whose name mentions `needle`, or
// empty.
//
// Matched case-insensitively, because the handle is NORMALISED: "Midi Through
// Port-0" on client 14 enumerates as "midi_through_midi_through_port-0_14_0".
// Searching for the friendly name finds nothing — which is the same mistake
// rust/clockwork-midi/tests/clock_accuracy.rs made, where it left four tests
// unable to pass on Linux, and which this test made too before it ever ran.
// Hence `needle` is spelled in the normalised vocabulary.
std::string findPort(const std::vector<std::string>& ports, const char* needle) {
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    const std::string want = lower(needle);
    for (const auto& p : ports)
        if (lower(p).find(want) != std::string::npos) return p;
    return {};
}

void sendOsc(ClockworkMidi* h, const char* addr, const char* arg, int32_t on) {
    char buf[512];
    osc::OutboundPacketStream s(buf, sizeof buf);
    s << osc::BeginMessage(addr) << arg << on << osc::EndMessage;
    clockwork_midi_handle_osc(h, reinterpret_cast<const uint8_t*>(s.Data()),
                       static_cast<uint32_t>(s.Size()));
}

} // namespace

TEST_CASE("midi clock: a generated tick reaches a real port and comes back",
          "[midi][clock][device]") {
    MidiCtx ctx;
    ClockworkMidi* midi = clockwork_midi_create(&ctx, emit_cb, clock_cb, transport_cb,
                                  "clockwork-clock-e2e");
    if (!midi) {
        WARN("no MIDI subsystem on this machine — skipping the end-to-end path");
        return;
    }
    // Enumerate first: creation does not scan the OS registry, so asking for
    // the ports before a refresh answers with an empty list.
    clockwork_midi_refresh(midi);
    clockwork_midi_emit_ports(midi);

    const std::string out = findPort(ctx.outPorts, "midi_through");
    const std::string in  = findPort(ctx.inPorts,  "midi_through");
    if (out.empty() || in.empty()) {
        std::string all = "ins:";
        for (const auto& p : ctx.inPorts)  all += " [" + p + "]";
        all += " outs:";
        for (const auto& p : ctx.outPorts) all += " [" + p + "]";
        WARN("no 'Midi Through' loopback port — skipping. enumerated " << all);
        clockwork_midi_destroy(midi);
        return;
    }
    INFO("loopback out=" << out << " in=" << in);

    // Listen on the far side of the loopback before sending anything.
    sendOsc(midi, CLOCKWORK_SYS("midi/in/enable"), in.c_str(), 1);

    // Opening the sink is what enables the OUTPUT port; MidiClockOut resolves
    // the same way from onBeat.
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkMidi, out.c_str(), 256);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);

    ClockworkClock clock;
    MidiClockOut m;

    // A SHORT beat, unlike the cases above: ticks now carry their time to the
    // platform, so a 24-second beat would really be scheduled 24 seconds out.
    // 240 ms puts the 24 pulses 10 ms apart.
    constexpr double kBeatSeconds = 0.24;
    const double t0 = clock.now();
    const uint64_t baseSent = m.sent();
    m.onBeat(clock, out, kBeatSeconds);
    m.generate(t0 + 1.0);            // release the whole beat to the sink

    CHECK(m.sent() - baseSent == MidiClockOut::kPulsesPerBeat);

    // Wait for the far side: the beat itself, plus the drain and the loopback.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ctx.pulses.load() < MidiClockOut::kPulsesPerBeat
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int got = ctx.pulses.load();
    INFO("pulses returned through the loopback: " << got);
    // Every pulse, and no duplicates — a fan-out bug that opened the port twice
    // would show here as 48.
    CHECK(got == MidiClockOut::kPulsesPerBeat);

    clockwork_sink_close(sink);
    clock.stopBackgroundWork();
    clockwork_midi_destroy(midi);
}
