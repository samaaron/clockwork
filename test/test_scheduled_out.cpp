// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_scheduled_out.cpp — a scheduled outbound message leaves at ITS time.
 *
 * A client sends every timed MIDI note and every timed OSC message the same
 * way: "/clockwork/schedule <timetag> <blob>", the blob being the plain
 * "/clockwork/midi/out/…" or "/clockwork/osc/send …" verb. Clockwork parks it,
 * fires it on the audio
 * thread at the start of the block that holds its time, and hands it to the
 * control thread. What these cases pin is the last hop: the message must
 * still carry `when` to the endpoint, so the endpoint (an OSC sink, a MIDI
 * port that timestamps) releases it at its moment — not at block time, and
 * not whenever the control thread got round to it.
 *
 * The engine is booted whole — routes, control thread, scheduler — but with
 * no audio device: manualAudioPump, so the test renders the blocks itself,
 * and freewheelClock, so the engine's clock is the sample count and nothing
 * else. Pumping 0.4 s of audio takes a few milliseconds of wall time, so the
 * engine's clock races ahead of the wall clock: an event due 0.3 s out fires
 * from the scheduler almost at once. That is the whole trick. A message that
 * leaves the moment it fires arrives ~0.3 s BEFORE its time on the wall clock;
 * one that carries its time is held by its endpoint and arrives at it.
 *
 * This is a separate executable from clockwork_tests. A ClockworkEngine and the
 * lanes fixture both own the process-global audio_processor state, and the
 * fixture never shuts down, so the two cannot share a process.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clock/clock_math.h"
#include "clockwork_event_sink.h"
#include "clockwork_prefix.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

using engine_test::Engine;
using engine_test::kSampleRate;

constexpr double kLeadSec = 0.3;   // how far ahead every case schedules

// "/clockwork/schedule <when> <inner>" — exactly what a client puts on the wire.
osc_test::Packet scheduled(double whenNtp, const osc_test::Packet& inner) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("schedule"))
        << static_cast<osc::int64>(clockwork::ntpToOscTimetag(whenNtp))
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    return b.end();
}

// A UDP socket on a free loopback port, with a timed receive.
struct Receiver {
    int      fd   = -1;
    uint16_t port = 0;

    Receiver() {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;
        REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0);
        socklen_t len = sizeof a;
        REQUIRE(getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0);
        port = ntohs(a.sin_port);
    }
    ~Receiver() {
#ifdef _WIN32
        closesocket(fd); WSACleanup();
#else
        close(fd);
#endif
    }

    // Blocks up to timeoutSec. On success fills `bytes` and returns the wall
    // NTP time the packet was read.
    bool receive(std::vector<uint8_t>& bytes, double& arrivedNtp, double timeoutSec) {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeoutSec * 1000);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
        timeval tv{};
        tv.tv_sec  = static_cast<long>(timeoutSec);
        tv.tv_usec = static_cast<long>((timeoutSec - tv.tv_sec) * 1e6);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        uint8_t buf[2048];
        const auto n = recv(fd, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (n <= 0) return false;
        arrivedNtp = wallClockNTP();
        bytes.assign(buf, buf + n);
        return true;
    }
};

// The MIDI sink whose target is `port`, if the engine opened one.
ClockworkSink midiSinkFor(const std::string& port) {
    ClockworkSink sinks[64];
    const uint32_t n = clockwork_sink_list(sinks, 64);
    for (uint32_t i = 0; i < n; ++i)
        if (clockwork_sink_kind(sinks[i]) == kClockworkSinkMidi && port == clockwork_sink_target(sinks[i]))
            return sinks[i];
    return CLOCKWORK_SINK_NONE;
}

#ifdef __APPLE__
// A CoreMIDI virtual destination: a MIDI output port the engine can open like
// any other, whose bytes land here. Each packet is kept with the timestamp
// CoreMIDI delivered it under (host time; 0 = "now") and the wall NTP time it
// was read at. Created before the engine boots, so it is in the first listing.
struct VirtualDestination {
    static constexpr const char* kName = "clockworkenginetest";

    struct Packet {
        std::vector<uint8_t> bytes;
        uint64_t             hostStamp = 0;
        double               readNtp   = 0.0;
    };
    MIDIClientRef   client = 0;
    MIDIEndpointRef dest   = 0;
    std::mutex          mu;
    std::vector<Packet> packets;

    VirtualDestination() {
        REQUIRE(MIDIClientCreate(CFSTR("clockwork-engine-tests"), nullptr, nullptr, &client) == noErr);
        REQUIRE(MIDIDestinationCreate(client, CFSTR("clockworkenginetest"), &VirtualDestination::onRead,
                                      this, &dest) == noErr);
    }
    ~VirtualDestination() {
        if (dest)   MIDIEndpointDispose(dest);
        if (client) MIDIClientDispose(client);
    }

    static void onRead(const MIDIPacketList* list, void* self, void*) {
        auto* me = static_cast<VirtualDestination*>(self);
        const double now = wallClockNTP();
        const MIDIPacket* p = &list->packet[0];
        std::lock_guard<std::mutex> lock(me->mu);
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            me->packets.push_back({std::vector<uint8_t>(p->data, p->data + p->length),
                                   p->timeStamp, now});
            p = MIDIPacketNext(p);
        }
    }

    // Host time → wall NTP, through the difference from now.
    static double hostToNtp(uint64_t host) {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        const double nowHost = static_cast<double>(mach_absolute_time());
        const double deltaNs = (static_cast<double>(host) - nowHost) * tb.numer / tb.denom;
        return wallClockNTP() + deltaNs / 1e9;
    }

    bool first(Packet& out) {
        std::lock_guard<std::mutex> lock(mu);
        if (packets.empty()) return false;
        out = packets.front();
        return true;
    }
};
#endif

} // namespace

TEST_CASE("scheduled OSC leaves at its time, not when the scheduler fired it",
          "[engine][schedule][osc]") {
    Engine e;
    Receiver rx;

    const auto inner = osc_test::message("/hello", 42);
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("osc/send"))
        << "127.0.0.1" << static_cast<int32_t>(rx.port)
        << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    const auto send = b.end();

    const double when = wallClockNTP() + kLeadSec;
    const auto   pkt  = scheduled(when, send);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);

    // The engine's clock passes `when` within milliseconds of wall time.
    e.pump(kLeadSec + 0.1);

    // Firing the event is not yet sending it: the audio thread hands it to the
    // control pass, which drains that ring on a block wake and only then
    // reaches the sink. pump() renders its 0.4 s in a few milliseconds and
    // stops, and this engine has only what the test renders — a running one
    // has a device supplying blocks forever. So keep the blocks coming for a
    // moment, as the MIDI case below and EngineFixture::reply() already do.
    // Without this the case failed on every run on Windows ARM64 (the send
    // never reached OscControl), though the block counter ticks after the
    // render, so the exact wake that was lost there is not yet pinned down.
    for (int i = 0; i < 20; ++i) {
        e.engine.pumpAudioBlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::vector<uint8_t> got;
    double arrived = 0.0;
    REQUIRE_FALSE(!rx.receive(got, arrived, kLeadSec + 2.0));   // it must arrive at all
    CHECK(got == inner.data);                                     // and be the inner message

    // The contract. A message that left when the scheduler fired it shows
    // up here ~0.3 s early; one that carried its time to the endpoint shows
    // up at it. 1 ms of slack for the two wall-clock reads on either side.
    INFO("arrived " << (when - arrived) * 1000.0 << " ms BEFORE its scheduled time");
    CHECK(arrived >= when - 0.001);
}

TEST_CASE("scheduled MIDI reaches its port carrying its time",
          "[engine][schedule][midi]") {
#ifdef __APPLE__
    VirtualDestination vd;
#endif
    Engine e;

    // Which output ports does this box have? Asked of the engine so the case
    // runs against the same list a client sees. On macOS the virtual
    // destination above is one of them; elsewhere the first port there is.
    const auto list = osc_test::message(CLOCKWORK_SYS("midi/ports/list"));
    e.engine.ingest(list.ptr(), list.size(), 0);
    osc_test::ParsedReply ports;
    REQUIRE(e.reply(CLOCKWORK_SYS("midi/ports.reply"), ports));
    // <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*
    const int nIn  = ports.argInt(0);
    const int nOut = ports.argInt(1 + nIn * 2);
    std::string port;
    for (int i = 0; i < nOut; ++i) {
        const std::string name = ports.argString(2 + nIn * 2 + i * 2);
        if (port.empty()) port = name;
#ifdef __APPLE__
        if (name.find(VirtualDestination::kName) != std::string::npos) port = name;
#endif
    }
#ifdef __APPLE__
    REQUIRE(port.find(VirtualDestination::kName) != std::string::npos);
#else
    if (port.empty()) {
        WARN("no MIDI output port on this box — skipping the MIDI case");
        return;
    }
#endif

    // Open the port, as a client that enables every port at boot does.
    {
        osc_test::Builder en;
        en.begin(CLOCKWORK_SYS("midi/out/enable")) << port.c_str() << static_cast<int32_t>(1);
        const auto p = en.end();
        e.engine.ingest(p.ptr(), p.size(), 0);
        e.pump(0.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // note_off: harmless on whatever real device is behind the port.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("midi/out/note_off"))
        << port.c_str() << static_cast<int32_t>(1) << static_cast<int32_t>(60)
        << static_cast<int32_t>(0);
    const auto noteOff = b.end();

    const double when = wallClockNTP() + kLeadSec;
    const auto   pkt  = scheduled(when, noteOff);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);
    e.pump(kLeadSec + 0.1);

#ifdef __APPLE__
    // The bytes themselves, at the destination. CoreMIDI hands a
    // timestamped packet to a virtual destination when it is SENT, stamped
    // with its time; an unstamped one (0) is "now". So the time the note
    // reached the port is its stamp if it has one, else when it was read —
    // and either way it must not be before `when`.
    VirtualDestination::Packet got;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!vd.first(got) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(vd.first(got));
    CHECK(got.bytes == std::vector<uint8_t>{0x80, 60, 0});
    const double reached = got.hostStamp ? VirtualDestination::hostToNtp(got.hostStamp)
                                         : got.readNtp;
    INFO("reached the port " << (when - reached) * 1000.0 << " ms BEFORE its scheduled time"
         << (got.hostStamp ? " (by its CoreMIDI stamp)" : " (unstamped: read time)"));
    CHECK(reached >= when - 0.001);
#endif

    // And the accounting. Sinks are it: PORTS.md — "a sink must record what went
    // out and when, or timing complaints become unfalsifiable". So a
    // scheduled note is one clockwork can account for: it went through a
    // sink for its port, and that sink first saw it BEFORE it was due
    // (`late == 0`, counted at first sight). Wait past `when` so a platform
    // that cannot timestamp (the sink holds it) has released it too.
    ClockworkSink sink = CLOCKWORK_SINK_NONE;
    ClockworkSinkStats st{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        sink = midiSinkFor(port);
        if (sink != CLOCKWORK_SINK_NONE && clockwork_sink_stats(sink, &st) && st.sent >= 1) break;
        e.engine.pumpAudioBlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    INFO("port '" << port << "'");
    REQUIRE(sink != CLOCKWORK_SINK_NONE);            // a scheduled note is accounted for on a sink
    CHECK(st.sent == 1);
    CHECK(st.late == 0);                       // it was seen before its time, not at it
    CHECK(st.dropped == 0);

}
