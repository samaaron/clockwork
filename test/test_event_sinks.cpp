// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_event_sinks.cpp — src/clockwork_event_sink.h through the C ABI, and through
 * the boundary a DSP actually reaches it by.
 *
 * WHAT IS PINNED HERE AND WHAT IS PINNED IN RUST
 *
 * The same split test_ports.cpp makes, for the same reason. The drain's own
 * arithmetic — that messages leave in time order however they arrived, that
 * `late` counts what could not be honoured, that a full sink refuses in
 * nanoseconds without allocating, that bytes arrive at a real socket byte for
 * byte — is pinned in `rust/clockwork-sinks/tests/`, where the allocator can
 * be counted and a loopback socket can be read.
 *
 * What is pinned HERE is everything a C caller can see: that a handle names a
 * sink and keeps naming it, that a closed or invented one refuses instead of
 * faulting, that every message is accounted for, and — the case this file
 * exists for — that a DSP given nothing but a handle and a send gets bytes out
 * of the process.
 *
 * WHY 127.0.0.1:9
 *
 * Port 9 is `discard`. It resolves, so the sink opens; a datagram sent to it
 * needs nobody listening, so a test asserting that a message LEFT does not
 * have to stand up a receiver in C++ on three platforms. That the bytes are
 * also correct on the wire is asserted in Rust, against a real socket.
 *
 * LAST IN THE FILE LIST on purpose, and for the same reason
 * test_stream_identity.cpp used to be: its final case rebuilds the DSP, which
 * restarts the placeholder's absolute frame counter, so anything asserting
 * pulse phase must already have run.
 */
#include "clockwork_event_sink.h"
#include "memory_profile.h"
#include "sink_profile.h"
#include "clock/clock_math.h"
#include "dsp_api.h"
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// Clockwork's own, as test_stream_identity.cpp reaches them: a device switch
// is what frees a DSP, and freeing a DSP is the only way to see clockwork
// close the sinks that DSP opened. Declared here rather than via
// audio_processor.h, which pulls in the <emscripten/...> shim that is not on a
// native include path. All three have C linkage (src/audio_processor.cpp).
extern "C" {
extern struct Dsp* g_dsp;
void destroy_dsp();
void rebuild_dsp(double sample_rate);
}

namespace {

// Not 0, so a reply that was broadcast rather than routed is visible.
constexpr uint32_t kClient = 0x51f00d;

// A destination that always resolves and never needs a listener. See the file
// comment.
constexpr const char* kDiscard = "127.0.0.1:9";

// The table is process-global, so nothing may leak out of a case.
struct Sinks {
    Sinks()  { lanes_test::boot(); clockwork_sink_close_all(); }
    ~Sinks() { clockwork_sink_close_all(); }
};

ClockworkSinkStats statsOf(ClockworkSink sink) {
    ClockworkSinkStats s{};
    REQUIRE(clockwork_sink_stats(sink, &s) != 0);
    return s;
}

// Wait until the drain thread has accounted for `n` messages, or give up.
// Polling is the honest shape: clockwork_event_sink.h has no drain call and no
// flush, precisely so that nothing has to be called for a sink to work.
//
// No REQUIRE inside the loop, deliberately: how many times round it goes
// depends on the scheduler, and an assertion in here would make the binary's
// total assertion count differ from run to run.
bool settled(ClockworkSink sink, uint64_t n, int max_ms = 3000) {
    for (int i = 0; i < max_ms; ++i) {
        ClockworkSinkStats s{};
        if (clockwork_sink_stats(sink, &s) != 0 && s.sent + s.dropped >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// ── The placeholder's sink verbs (dsp/dummy/dummy_dsp.cpp) ──────────────────

// Ask the DSP to open a sink and report the handle it got. One tick carries
// the message and the reply.
ClockworkSink dspOpenSink(const char* target, ClockworkSinkKind kind, uint32_t capacity) {
    lanes_test::drainRt();
    osc_test::Builder b;
    auto& s = b.begin("/dummy/sink/open");
    s << target << static_cast<osc::int32>(kind)
      << static_cast<osc::int32>(capacity);
    const auto pkt = b.end();
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));

    for (const auto& f : lanes_test::tickUntil(4, "/dummy/sink/opened")) {
        const auto r = osc_test::parseReply(f.data.data(),
                                            static_cast<uint32_t>(f.data.size()));
        if (r.address != "/dummy/sink/opened") continue;
        REQUIRE(f.sourceId == kClient);   // routed to the asker, not broadcast
        REQUIRE(r.argCount() == 1);
        return static_cast<ClockworkSink>(r.argInt(0));
    }
    FAIL("the DSP never answered /dummy/sink/open");
    return CLOCKWORK_SINK_NONE;
}

// Queue one send in the DSP. It goes out at the top of the NEXT dsp_process,
// dated by that block's block_time — which is the whole point of the exercise.
void dspQueueSend(ClockworkSink sink, const char* text, uint32_t delay_ms) {
    osc_test::Builder b;
    auto& s = b.begin("/dummy/sink/send");
    s << static_cast<osc::int32>(sink) << text
      << static_cast<osc::int32>(delay_ms);
    const auto pkt = b.end();
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));
}

// How many sends the DSP is still holding for a block_time.
int32_t dspPending() {
    lanes_test::drainRt();
    const auto msg = osc_test::message("/dummy/sink/pending");
    REQUIRE(lanes_test::ingress(msg.ptr(), msg.size(), kClient));
    for (const auto& f : lanes_test::tickUntil(4, "/dummy/sink/still")) {
        const auto r = osc_test::parseReply(f.data.data(),
                                            static_cast<uint32_t>(f.data.size()));
        if (r.address != "/dummy/sink/still") continue;
        return r.argInt(0);
    }
    FAIL("the DSP never answered /dummy/sink/pending");
    return -1;
}

} // namespace

// ── The slot ────────────────────────────────────────────────────────────────

TEST_CASE("sinks: a sink is a slot that keeps its identity until it is closed",
          "[sinks]") {
    Sinks guard;

    ClockworkSink list[8] = {};
    REQUIRE(clockwork_sink_list(list, 8) == 0);

    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, kDiscard, 64);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    REQUIRE(clockwork_sink_is_open(sink) == 1);
    REQUIRE(clockwork_sink_kind(sink) == kClockworkSinkOsc);
    REQUIRE(clockwork_sink_target(sink) != nullptr);
    REQUIRE(std::string(clockwork_sink_target(sink)) == kDiscard);

    // A new sink has done nothing, and says so in all four numbers.
    const ClockworkSinkStats fresh = statsOf(sink);
    REQUIRE(fresh.sent == 0);
    REQUIRE(fresh.dropped == 0);
    REQUIRE(fresh.late == 0);
    REQUIRE(fresh.scheduled == 0);

    REQUIRE(clockwork_sink_list(list, 8) == 1);
    REQUIRE(list[0] == sink);

    clockwork_sink_close(sink);
    REQUIRE(clockwork_sink_is_open(sink) == 0);
    REQUIRE(clockwork_sink_list(list, 8) == 0);
}

TEST_CASE("sinks: a closed or invented handle refuses rather than faulting",
          "[sinks]") {
    Sinks guard;
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, kDiscard, 64);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    clockwork_sink_close(sink);
    clockwork_sink_close(sink);       // twice is safe

    const uint8_t body[3] = { 0x90, 60, 100 };
    for (ClockworkSink bad : { sink, CLOCKWORK_SINK_NONE, ClockworkSink(1),
                         ClockworkSink(0xdeadbeefu), ClockworkSink(0xffffffffu) }) {
        INFO("handle " << bad);
        REQUIRE(clockwork_sink_is_open(bad) == 0);
        REQUIRE(clockwork_sink_kind(bad) == 0);
        REQUIRE(clockwork_sink_target(bad) == nullptr);
        REQUIRE(clockwork_sink_send(bad, body, 3, 1) == 0);

        // A refused stats call leaves the caller's struct alone: a caller that
        // ignored the return would otherwise read zeros as facts.
        ClockworkSinkStats s{ 7, 7, 7, 7 };
        REQUIRE(clockwork_sink_stats(bad, &s) == 0);
        REQUIRE(s.sent == 7);
        REQUIRE(s.dropped == 7);
        REQUIRE(s.late == 7);
        REQUIRE(s.scheduled == 7);
    }
}

TEST_CASE("sinks: a destination that is not there is refused at open",
          "[sinks]") {
    Sinks guard;
    // The refusal is the feature. A sink that opened onto nothing would accept
    // every message, deliver none, and report `sent` for all of them — which
    // is the silent-drop shape clockwork has been bitten by before.
    REQUIRE(clockwork_sink_open(kClockworkSinkOsc, "not-a-destination", 64) == CLOCKWORK_SINK_NONE);
    REQUIRE(clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:0", 64) == CLOCKWORK_SINK_NONE);
    REQUIRE(clockwork_sink_open(kClockworkSinkOsc, nullptr, 64) == CLOCKWORK_SINK_NONE);
    REQUIRE(clockwork_sink_open(static_cast<ClockworkSinkKind>(7), kDiscard, 64) == CLOCKWORK_SINK_NONE);
    // No MIDI subsystem has been created in this binary, so there is no port
    // to open onto and no sink to be had.
    REQUIRE(clockwork_sink_open(kClockworkSinkMidi, "anything", 64) == CLOCKWORK_SINK_NONE);
    // Nothing was left behind by any of that.
    ClockworkSink list[8] = {};
    REQUIRE(clockwork_sink_list(list, 8) == 0);
}

TEST_CASE("sinks: every message is either sent or dropped, and none is lost",
          "[sinks]") {
    Sinks guard;
    // The accounting invariant, which is what makes the four numbers worth
    // reading at all: offer far more than the sink can hold, and every single
    // one is in exactly one column. A sink that quietly swallowed the overflow
    // would report a total short of what it was given.
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, kDiscard, 8);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);

    constexpr uint64_t kSends = 20000;
    const uint8_t body[3] = { 0x90, 60, 100 };
    uint64_t accepted = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < kSends; ++i) {
        if (clockwork_sink_send(sink, body, 3, 1) != 0) ++accepted;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Nothing waited. A send that took a lock or a syscall could not do
    // twenty thousand of these inside a second on any machine this runs on.
    REQUIRE(elapsed < std::chrono::seconds(1));
    // The sink is genuinely shallower than the offer, so the drop path really
    // was taken rather than the whole lot fitting.
    REQUIRE(accepted < kSends);

    REQUIRE(settled(sink, kSends));
    const ClockworkSinkStats s = statsOf(sink);
    REQUIRE(s.sent + s.dropped == kSends);
    REQUIRE(s.sent == accepted);
    REQUIRE(s.dropped == kSends - accepted);
    // Nothing was scheduled: no native backend has a timestamped send, and
    // UDP has no such thing at all.
    REQUIRE(s.scheduled == 0);
    clockwork_sink_close(sink);
}

// ── The boundary: a DSP with a handle and a send ────────────────────────────────

// The build's OSC shape, parsed from the same string the engine installs at
// boot, so these cases hold under a CMake override or an embedded profile as
// well as the desktop default.
namespace {
struct OscShape {
    uint32_t widths[CLOCKWORK_SINK_CLASSES_MAX] = {};
    uint32_t depths[CLOCKWORK_SINK_CLASSES_MAX] = {};
    uint32_t n = 0;
    uint32_t baseWidth() const { uint32_t w = ~0u; for (uint32_t i = 0; i < n; ++i) w = std::min(w, widths[i]); return w; }
    uint32_t widest() const    { uint32_t w = 0;   for (uint32_t i = 0; i < n; ++i) w = std::max(w, widths[i]); return w; }
    uint32_t baseDepth() const {
        uint32_t d = 0;
        for (uint32_t i = 0; i < n; ++i) if (widths[i] == baseWidth()) d = depths[i];
        d = std::max(d, 2u); while (d & (d - 1)) ++d;   // as the sink rounds it
        return d;
    }
};
OscShape oscShape() {
    OscShape sh;
    sh.n = clockwork_parse_sink_classes(CLOCKWORK_OSC_SINK_CLASSES, sh.widths, sh.depths,
                                        CLOCKWORK_SINK_CLASSES_MAX);
    return sh;
}
uint32_t roundedDepth(uint32_t d) { d = std::max(d, 2u); while (d & (d - 1)) ++d; return d; }
}

TEST_CASE("sinks: a message wider than the base cell rides a wider class",
          "[sinks][classes]") {
    // A message the base cell refuses is carried by a wider class, up to the
    // widest cell exactly. Both must LEAVE, not merely be accepted: sent
    // counts what reached the endpoint.
    const OscShape sh = oscShape();
    REQUIRE(sh.n >= 2);   // the shape has a wider class to ride
    REQUIRE(clockwork_install_sink_profiles() != 0);   // no engine in this binary
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, kDiscard, 0);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    REQUIRE(clockwork_sink_max_message_bytes(sink) == sh.widest());

    // The widest cell, or the widest datagram if the cell is wider: the wire
    // is the endpoint's limit, and a message the socket refuses is a drop.
    const uint32_t wireWidest = std::min(sh.widest(), CLOCKWORK_MAX_DATAGRAM_BYTES);
    std::vector<uint8_t> mid(sh.baseWidth() + 1, 0xAB), big(wireWidest, 0xCD);
    REQUIRE(clockwork_sink_send(sink, mid.data(), static_cast<uint32_t>(mid.size()), 1) != 0);
    REQUIRE(clockwork_sink_send(sink, big.data(), static_cast<uint32_t>(big.size()), 1) != 0);

    ClockworkSinkStats st{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (clockwork_sink_stats(sink, &st) != 0 && st.sent + st.dropped >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(st.sent == 2);
    CHECK(st.dropped == 0);
    clockwork_sink_close(sink);
}

TEST_CASE("sinks: a message wider than the widest class is refused whole and counted",
          "[sinks][classes]") {
    REQUIRE(clockwork_install_sink_profiles() != 0);
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, kDiscard, 0);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    const uint32_t widest = clockwork_sink_max_message_bytes(sink);
    std::vector<uint8_t> over(widest + 1, 0xEE);
    CHECK(clockwork_sink_send(sink, over.data(), static_cast<uint32_t>(over.size()), 1) == 0);
    ClockworkSinkStats st{};
    REQUIRE(clockwork_sink_stats(sink, &st) != 0);
    CHECK(st.dropped == 1);
    CHECK(st.sent == 0);
    // Exactly the widest cell fits.
    CHECK(clockwork_sink_send(sink, over.data(), widest, 1) != 0);
    clockwork_sink_close(sink);
}

TEST_CASE("sinks: capacity at open sizes the base class and leaves the rest",
          "[sinks][classes]") {
    const OscShape sh = oscShape();
    REQUIRE(clockwork_install_sink_profiles() != 0);
    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkOsc, kDiscard, 0);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    // What the profile reserves is pinned in test_sink_profile.cpp; here the
    // question is what `capacity` adds: base cells, at the base width, and
    // nothing else.
    const uint32_t asked = 64;
    const ClockworkSink deeper = clockwork_sink_open(kClockworkSinkOsc, kDiscard, asked);
    REQUIRE(deeper != CLOCKWORK_SINK_NONE);
    const uint64_t added = uint64_t(roundedDepth(asked)) * sh.baseWidth()
                         - uint64_t(sh.baseDepth()) * sh.baseWidth();
    CHECK(clockwork_sink_bytes_reserved(deeper) == clockwork_sink_bytes_reserved(sink) + added);
    CHECK(clockwork_sink_max_message_bytes(deeper) == clockwork_sink_max_message_bytes(sink));
    clockwork_sink_close(deeper);
    clockwork_sink_close(sink);
}

TEST_CASE("sinks: the DSP opens a sink and gets a handle back", "[sinks][boundary]") {
    Sinks guard;
    const ClockworkSink sink = dspOpenSink(kDiscard, kClockworkSinkOsc, 64);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);

    // The handle the DSP was given names the sink clockwork holds — the
    // whole content of "the DSP emits to a handle, not to an address".
    REQUIRE(clockwork_sink_is_open(sink) == 1);
    REQUIRE(clockwork_sink_kind(sink) == kClockworkSinkOsc);
    REQUIRE(std::string(clockwork_sink_target(sink)) == kDiscard);

    // And a destination it cannot have is refused all the way back to the DSP,
    // which reports CLOCKWORK_SINK_NONE rather than a handle that does nothing.
    REQUIRE(dspOpenSink("not-a-destination", kClockworkSinkOsc, 64) == CLOCKWORK_SINK_NONE);
}

TEST_CASE("sinks: what the DSP sends leaves the process", "[sinks][boundary][audio]") {
    Sinks guard;
    const ClockworkSink sink = dspOpenSink(kDiscard, kClockworkSinkOsc, 64);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    REQUIRE(statsOf(sink).sent == 0);

    // Queued by dsp_osc, emitted from dsp_process. Nothing has been sent yet:
    // the DSP is holding it for a block_time to date it by, which is the shape
    // a real scheduling DSP has.
    dspQueueSend(sink, "from-the-audio-thread", 0);
    REQUIRE(dspPending() == 1);

    // One block, and it is gone from the DSP.
    lanes_test::tick();
    REQUIRE(dspPending() == 0);

    // ...and out of the process. Nothing in this test drains anything: the
    // header has no drain call, so a sink that needed one would never deliver.
    REQUIRE(settled(sink, 1));
    const ClockworkSinkStats s = statsOf(sink);
    REQUIRE(s.sent == 1);
    REQUIRE(s.dropped == 0);
    // A zero delay becomes OSC's "immediately", which cannot be late.
    REQUIRE(s.late == 0);
}

TEST_CASE("sinks: a DSP that gives a time gets it honoured or told",
          "[sinks][boundary][audio]") {
    Sinks guard;
    const ClockworkSink sink = dspOpenSink(kDiscard, kClockworkSinkOsc, 64);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);

    // 40 ms past this block's start, computed by the DSP from block_time —
    // the arithmetic dsp_api.h says is the DSP's own.
    dspQueueSend(sink, "twenty-past", 40);
    lanes_test::tick();
    REQUIRE(settled(sink, 1));

    const ClockworkSinkStats s = statsOf(sink);
    REQUIRE(s.sent == 1);
    REQUIRE(s.dropped == 0);
    // AND IT IS LATE, which is correct and worth stating rather than
    // asserting away. LanesFixture self-clocks from kBaseNtp (the 1900→1970
    // offset, i.e. midnight on 1 January 1970) so that a test can name the
    // exact instant a given absolute frame is rendered at. The sink compares
    // against the wall clock, because that is the domain a real block_time is
    // in. So a message the DSP dates "40 ms after this block" is, in this
    // fixture, more than half a century overdue — the sink sends it at once
    // and says so in the one number the header calls the one worth watching.
    // A sink that reported 0 here would be a sink whose `late` never fires.
    REQUIRE(s.late == 1);
}

TEST_CASE("sinks: freeing the DSP closes the sinks it opened",
          "[sinks][boundary][audio]") {
    // LAST in this file, and this file last in the list: rebuilding the DSP
    // restarts the placeholder's absolute frame counter.
    Sinks guard;
    const ClockworkSink sink = dspOpenSink(kDiscard, kClockworkSinkOsc, 64);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    REQUIRE(clockwork_sink_is_open(sink) == 1);

    // dsp_api.h gives a DSP no close, on the grounds that its sinks last
    // exactly as long as it does. This is the half of that promise clockwork
    // keeps. Without it a device switch would leak a slot per sink per switch,
    // and the leak would present as sinks that stop opening after a while
    // rather than as anything a log would show.
    destroy_dsp();
    REQUIRE(clockwork_sink_is_open(sink) == 0);
    ClockworkSink list[8] = {};
    REQUIRE(clockwork_sink_list(list, 8) == 0);

    rebuild_dsp(lanes_test::kSampleRate);
    REQUIRE(g_dsp != nullptr);
    // A fresh instance holds no sinks, and the old handle cannot reach one.
    const uint8_t body[1] = { 0x00 };
    REQUIRE(clockwork_sink_send(sink, body, 1, 1) == 0);
    REQUIRE(clockwork_sink_list(list, 8) == 0);

    // And the new instance can open its own, on a slot the closed one left.
    const ClockworkSink again = dspOpenSink(kDiscard, kClockworkSinkOsc, 64);
    REQUIRE(again != CLOCKWORK_SINK_NONE);
    REQUIRE(again != sink);     // a reopened slot never reissues a handle
}

// ── Cancelling ───────────────────────────────────────────────────────────────

namespace {
// Now, as an OSC 32.32 timetag — the same domain clockwork_sink_send's `when` is in.
// clock_math.h is where that conversion is spelled for the whole tree.
int64_t sink_now() {
    return clockwork::ntpToOscTimetag(wallClockNTP());   // global by design; see clock_math.h
}
}  // namespace
//
// Through the C ABI, because that is how a consumer reaches it: a run-stop is
// a client or a host calling clockwork_sink_flush_all(), not Rust calling Rust.

TEST_CASE("sinks: a flush cancels what is pending and says how many",
          "[sinks][flush]") {
    clockwork_sink_close_all();
    const ClockworkSink s = clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:57999", 64);
    REQUIRE(s != CLOCKWORK_SINK_NONE);

    // Far enough out that no look-ahead reaches them, so they are all still
    // the sink's to cancel.
    const int64_t distant = sink_now() + (int64_t)(1ull << 32) * 60;
    const uint8_t msg[4] = {'/','x',0,0};
    for (int i = 0; i < 5; ++i)
        REQUIRE(clockwork_sink_send(s, msg, sizeof msg, distant + i) != 0);

    CHECK(clockwork_sink_flush(s) == 5);

    ClockworkSinkStats st{};
    REQUIRE(clockwork_sink_stats(s, &st));
    CHECK(st.cancelled == 5);
    // A cancellation is not a failure to carry: counting it as a drop would
    // make a working run-stop look like a broken sink.
    CHECK(st.dropped == 0);
    CHECK(st.sent == 0);

    clockwork_sink_close(s);
}

TEST_CASE("sinks: flush_all reaches every sink, whoever queued them",
          "[sinks][flush]") {
    // The reason cancellation belongs on the sink rather than on a queue: a
    // guest sending through clockwork_sink_send and a client sending verbs end up in
    // the same place, so one call covers both.
    clockwork_sink_close_all();
    const ClockworkSink a = clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:57998", 32);
    const ClockworkSink b = clockwork_sink_open(kClockworkSinkOsc, "127.0.0.1:57997", 32);
    REQUIRE(a != CLOCKWORK_SINK_NONE);
    REQUIRE(b != CLOCKWORK_SINK_NONE);

    const int64_t distant = sink_now() + (int64_t)(1ull << 32) * 60;
    const uint8_t msg[4] = {'/','y',0,0};
    REQUIRE(clockwork_sink_send(a, msg, sizeof msg, distant) != 0);
    REQUIRE(clockwork_sink_send(a, msg, sizeof msg, distant + 1) != 0);
    REQUIRE(clockwork_sink_send(b, msg, sizeof msg, distant) != 0);

    CHECK(clockwork_sink_flush_all() == 3);

    ClockworkSinkStats st{};
    REQUIRE(clockwork_sink_stats(a, &st)); CHECK(st.cancelled == 2);
    REQUIRE(clockwork_sink_stats(b, &st)); CHECK(st.cancelled == 1);

    clockwork_sink_close(a);
    clockwork_sink_close(b);
}

TEST_CASE("sinks: flushing a closed or invented handle answers 0",
          "[sinks][flush]") {
    CHECK(clockwork_sink_flush(CLOCKWORK_SINK_NONE) == 0);
    CHECK(clockwork_sink_flush(999999u) == 0);
}
