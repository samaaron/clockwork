// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_host_forward.cpp — the host as the end of clockwork's chain.
 *
 * On a host with no NRT thread, a /clockwork/ verb the audio thread does not
 * answer itself has nowhere to go. Until now it was refused there. A host
 * may instead have a FRONT — a thread of its own that answers the control
 * verbs, which on the web is the client's main thread, the only place Web
 * MIDI and the Gamepad API exist — and with clockwork_host_forward(1) the
 * audio thread forwards to it: the verb goes back out over the egress, to
 * the origin that sent it, wrapped in a bundle carrying the call's time.
 *
 * Driven through the lanes ABI, which is exactly the shape of the worklet:
 * bytes in the IN ring, one block rendered, frames off the RT egress ring.
 * What these pin:
 *
 *   - forwarding off (a bare host's default): an unknown verb is refused as
 *     it always was, to its sender;
 *   - forwarding on: the same verb comes back to its sender, whole, inside a
 *     bundle whose timetag is 1 — it never waited;
 *   - a verb the scheduler fired comes back inside a bundle whose timetag is
 *     the moment it was scheduled for, not the block it fired in;
 *   - ping is answered on the audio thread either way: liveness does not
 *     move to the host with the rest;
 *   - the switch is a switch, and turning it off restores the refusal.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "clock/clock_math.h"
#include "clockwork_prefix.h"
#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kClient = 0x40f7;   // not 0: 0 is the broadcast route

// What a forwarded frame looks like once taken apart.
struct Forwarded {
    bool        isBundle = false;
    uint64_t    timetag  = 0;
    std::string address;               // of the one element inside
    std::vector<uint8_t> element;
};

Forwarded unwrap(const lanes_test::EgressFrame& f) {
    Forwarded out;
    const auto& d = f.data;
    if (d.size() < 20 || std::memcmp(d.data(), "#bundle", 8) != 0) return out;
    out.isBundle = true;
    for (int i = 0; i < 8; ++i) out.timetag = (out.timetag << 8) | d[8 + i];
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n = (n << 8) | d[16 + i];
    if (20 + n > d.size()) return out;
    out.element.assign(d.begin() + 20, d.begin() + 20 + n);
    out.address = osc_test::parseAddress(out.element.data(), n);
    return out;
}

// "/clockwork/midi/out/note_on ,siii * ch note vel" — the note every case sends.
osc_test::Packet noteOn(int32_t note, int32_t vel) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/out/note_on"))
        << "*" << static_cast<osc::int32>(1) << static_cast<osc::int32>(note)
        << static_cast<osc::int32>(vel);
    return b.end();
}

const lanes_test::EgressFrame* find(const std::vector<lanes_test::EgressFrame>& frames,
                                    const char* address) {
    for (const auto& f : frames) {
        if (osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    }
    return nullptr;
}

const lanes_test::EgressFrame* findBundle(const std::vector<lanes_test::EgressFrame>& frames) {
    for (const auto& f : frames)
        if (f.data.size() >= 8 && std::memcmp(f.data.data(), "#bundle", 8) == 0) return &f;
    return nullptr;
}

// Forwarding off for the rest of the binary, whatever a case did.
struct ForwardOff { ~ForwardOff() { clockwork_host_forward(0); } };

} // namespace

TEST_CASE("host forward: off, an unknown verb is refused on the audio thread",
          "[host_forward][lanes]") {
    lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(0);
    REQUIRE(clockwork_host_forwards() == 0);
    lanes_test::drainRt();

    const auto verb = noteOn(60, 100);
    REQUIRE(lanes_test::ingress(verb.ptr(), verb.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    const auto* err = find(frames, CLOCKWORK_SYS("error"));
    REQUIRE(err != nullptr);
    CHECK(err->sourceId == kClient);
    const auto p = osc_test::parseReply(err->data.data(), static_cast<uint32_t>(err->data.size()));
    CHECK(p.argString(0) == CLOCKWORK_SYS("midi/out/note_on"));
    CHECK(findBundle(frames) == nullptr);
}

TEST_CASE("host forward: on, the verb goes back to its sender whole, inside an immediate bundle",
          "[host_forward][lanes]") {
    lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(1);
    REQUIRE(clockwork_host_forwards() == 1);
    lanes_test::drainRt();

    const auto verb = noteOn(60, 100);
    REQUIRE(lanes_test::ingress(verb.ptr(), verb.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, nullptr);
    const auto* fwd = findBundle(frames);
    REQUIRE(fwd != nullptr);
    CHECK(fwd->sourceId == kClient);

    const Forwarded f = unwrap(*fwd);
    REQUIRE(f.isBundle);
    CHECK(f.timetag == 1);                          // never waited
    CHECK(f.address == CLOCKWORK_SYS("midi/out/note_on"));
    CHECK(f.element == verb.data);                  // byte for byte, as sent
    CHECK(find(frames, CLOCKWORK_SYS("error")) == nullptr);   // no refusal
}

TEST_CASE("host forward: a verb nobody would answer is forwarded too — the host is the far end",
          "[host_forward][lanes]") {
    // The audio thread does not decide what the host answers: an address it
    // has never heard of goes to the host, whose front refuses it. That is
    // the native shape too (the refusal at the far end of the control hop).
    lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(1);
    lanes_test::drainRt();

    const auto verb = osc_test::message(CLOCKWORK_SYS("no/such/verb"));
    REQUIRE(lanes_test::ingress(verb.ptr(), verb.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, nullptr);
    const auto* fwd = findBundle(frames);
    REQUIRE(fwd != nullptr);
    CHECK(unwrap(*fwd).address == CLOCKWORK_SYS("no/such/verb"));
    CHECK(find(frames, CLOCKWORK_SYS("error")) == nullptr);
}

TEST_CASE("host forward: ping is answered on the audio thread either way",
          "[host_forward][lanes]") {
    lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(1);
    lanes_test::drainRt();

    const auto ping = osc_test::message(CLOCKWORK_SYS("ping"), 77);
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("pong"));
    const auto* pong = find(frames, CLOCKWORK_SYS("pong"));
    REQUIRE(pong != nullptr);
    CHECK(pong->sourceId == kClient);
    const auto p = osc_test::parseReply(pong->data.data(), static_cast<uint32_t>(pong->data.size()));
    CHECK(p.argInt(0) == 77);
    CHECK(findBundle(frames) == nullptr);          // not forwarded as well
}

#if CLOCKWORK_SCHEDULER
TEST_CASE("host forward: a verb the scheduler fired carries the moment it was scheduled for",
          "[host_forward][lanes][schedule]") {
    // The fixture self-clocks from frame 0, so the exact NTP time a block is
    // rendered at is known in advance. Schedule a note four blocks out and
    // tick past it: the forwarded bundle's timetag must be THAT moment,
    // not the start of whichever block fired it.
    const uint32_t block = lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(1);
    lanes_test::drainRt();

    const uint64_t now      = lanes_test::tick();          // a known frame
    const uint64_t dueFrame = now + 4 * block + block / 2; // mid-block, on purpose
    const double   dueNtp   = lanes_test::ntpAtFrame(dueFrame);
    const uint64_t dueTag   = clockwork::ntpToOscTimetag(dueNtp);

    const auto inner = noteOn(64, 90);
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("schedule"))
        << static_cast<osc::int64>(dueTag)
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    const auto pkt = b.end();
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));

    // Not yet: three blocks in, nothing has fired.
    auto early = lanes_test::tickUntil(3, nullptr);
    CHECK(findBundle(early) == nullptr);

    const auto frames = lanes_test::tickUntil(4, nullptr);
    const auto* fwd = findBundle(frames);
    REQUIRE(fwd != nullptr);
    CHECK(fwd->sourceId == kClient);
    const Forwarded f = unwrap(*fwd);
    CHECK(f.address == CLOCKWORK_SYS("midi/out/note_on"));
    CHECK(f.element == inner.data);
    CHECK(f.timetag == dueTag);
}
#endif

TEST_CASE("host forward: off again, the refusal is back", "[host_forward][lanes]") {
    lanes_test::boot();
    clockwork_host_forward(1);
    clockwork_host_forward(0);
    lanes_test::drainRt();

    const auto verb = osc_test::message(CLOCKWORK_SYS("gamepad/devices/list"));
    REQUIRE(lanes_test::ingress(verb.ptr(), verb.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    REQUIRE(find(frames, CLOCKWORK_SYS("error")) != nullptr);
    CHECK(findBundle(frames) == nullptr);
}

// ── A guest's own sends: the host as the sink's endpoint ─────────────────────
//
// A self-directed guest never sends through ingress: it opens a sink and
// sends with its own `when` (clockwork_event_sink.h). On a target with no
// MIDI port and no thread — the worklet — the host is that sink's endpoint
// (clockwork_sink_set_host_emit), installed by the same switch: the send
// goes out as "/clockwork/midi/sink/send ,sb <port> <bytes>" inside a bundle
// carrying its time, for the host's front to put on the port with that time.

#include "clockwork_event_sink.h"

namespace {

struct SinkSend { std::string port; std::vector<uint8_t> bytes; };

// The port and bytes out of a forwarded "/clockwork/midi/sink/send".
SinkSend unwrapSinkSend(const Forwarded& f) {
    SinkSend out;
    const auto r = osc_test::parseReply(f.element.data(), static_cast<uint32_t>(f.element.size()));
    REQUIRE(r.argCount() == 2);
    out.port = r.argString(0);
    out.bytes = r.argBlob(1);
    return out;
}

} // namespace

TEST_CASE("host forward: on, a MIDI sink opens onto the host and a send goes out with its time",
          "[host_forward][lanes][sinks]") {
    lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(1);
    lanes_test::drainRt();

    const ClockworkSink sink = clockwork_sink_open(kClockworkSinkMidi, "Fake Synth", 0);
    REQUIRE(sink != CLOCKWORK_SINK_NONE);
    CHECK(clockwork_sink_kind(sink) == kClockworkSinkMidi);

    const int64_t when = static_cast<int64_t>(
        clockwork::ntpToOscTimetag(lanes_test::ntpAtFrame(480000)));
    const uint8_t note[3] = { 0x90, 60, 100 };
    REQUIRE(clockwork_sink_send(sink, note, 3, when) != 0);

    // DIRECT: the frame is on the egress before any block is rendered.
    const auto frames = lanes_test::drainRt();
    const auto* fwd = findBundle(frames);
    REQUIRE(fwd != nullptr);
    CHECK(fwd->sourceId == 0);                       // a sink has no caller
    const Forwarded f = unwrap(*fwd);
    CHECK(f.timetag == static_cast<uint64_t>(when));
    CHECK(f.address == CLOCKWORK_SYS("midi/sink/send"));
    const SinkSend s = unwrapSinkSend(f);
    CHECK(s.port == "Fake Synth");
    CHECK(s.bytes == std::vector<uint8_t>(note, note + 3));

    // Counted as handed to a platform that holds it.
    ClockworkSinkStats st{};
    REQUIRE(clockwork_sink_stats(sink, &st) != 0);
    CHECK(st.sent == 1);
    CHECK(st.scheduled == 1);
    CHECK(st.dropped == 0);
    clockwork_sink_close(sink);
}

TEST_CASE("host forward: off, a MIDI port this build cannot reach is refused at open, as before",
          "[host_forward][lanes][sinks]") {
    lanes_test::boot();
    clockwork_host_forward(0);
    CHECK(clockwork_sink_open(kClockworkSinkMidi, "No Such Port, Surely", 0) == CLOCKWORK_SINK_NONE);
}

TEST_CASE("host forward: what the guest sends itself reaches the host with the moment it meant",
          "[host_forward][lanes][sinks][boundary]") {
    // The whole path a self-directed guest takes: open through DspHost, send
    // from dsp_process dated by the block, and out to the host with that
    // time — never through ingress, never through a scheduler.
    const uint32_t block = lanes_test::boot();
    ForwardOff guard;
    clockwork_host_forward(1);
    lanes_test::drainRt();

    osc_test::Builder open;
    open.begin("/dummy/sink/open") << "Fake Synth" << static_cast<osc::int32>(kClockworkSinkMidi)
                                   << static_cast<osc::int32>(0);
    const auto openPkt = open.end();
    REQUIRE(lanes_test::ingress(openPkt.ptr(), openPkt.size(), kClient));
    const auto opened = lanes_test::tickUntil(4, "/dummy/sink/opened");
    const auto* rep = find(opened, "/dummy/sink/opened");
    REQUIRE(rep != nullptr);
    const auto sink = static_cast<ClockworkSink>(
        osc_test::parseReply(rep->data.data(), static_cast<uint32_t>(rep->data.size())).argInt(0));
    REQUIRE(sink != CLOCKWORK_SINK_NONE);

    const uint8_t note[3] = { 0x90, 0x3c, 0x64 };
    osc_test::Builder send;
    send.begin("/dummy/sink/send") << static_cast<osc::int32>(sink)
                                   << osc::Blob(note, 3) << static_cast<osc::int32>(250);
    const auto sendPkt = send.end();
    lanes_test::drainRt();
    REQUIRE(lanes_test::ingress(sendPkt.ptr(), sendPkt.size(), kClient));
    const uint64_t frame = lanes_test::tick();   // the block that drained it and ran the send
    const auto frames = lanes_test::drainRt();
    const auto* fwd = findBundle(frames);
    REQUIRE(fwd != nullptr);
    const Forwarded f = unwrap(*fwd);
    CHECK(f.address == CLOCKWORK_SYS("midi/sink/send"));
    const SinkSend s = unwrapSinkSend(f);
    CHECK(s.port == "Fake Synth");
    CHECK(s.bytes == std::vector<uint8_t>(note, note + 3));

    // Dated by that block's start plus 250 ms, to within one block.
    const double blockNtp = lanes_test::ntpAtFrame(frame);
    const double gotNtp   = clockwork::oscTimetagToNtp(static_cast<int64_t>(f.timetag));
    INFO("sent for " << (gotNtp - blockNtp) * 1000.0 << " ms after the block");
    CHECK(gotNtp >= blockNtp + 0.250 - 0.001);
    CHECK(gotNtp <= blockNtp + 0.250 + static_cast<double>(block) / lanes_test::kSampleRate + 0.001);

    clockwork_sink_close(sink);
}
