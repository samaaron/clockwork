// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_embed_midi.cpp — a host that supplies the MIDI itself (a DAW handing
 * a plugin its events), through clockwork_embed.h.
 *
 * In: bytes with a frame reach the client as the event any port's bytes
 * become, at the time of that frame. Out: a client's send to the host's port
 * comes back to the host in the render that holds its time, at its frame.
 * Both whatever the host's buffers and the engine's block.
 *
 * Every engine here keeps time from its frame count (time_origin_ntp), so a
 * frame's time is exact and nothing waits on a clock.
 */
#include <catch2/catch_test_macros.hpp>
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "clockwork_embed.h"
#include "clock/timetag.h"
#include "lanes/lanes.h"
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr double kOrigin = 1000.0;

struct Host {
    ClockworkEmbed* h = nullptr;
    double rate = 0.0;
    uint64_t frames = 0;   // rendered so far
    explicit Host(const char* in = "daw", const char* out = "daw") {
        ClockworkEmbedConfig c {};
        c.struct_bytes      = sizeof c;
        c.sample_rate       = lanes_test::kSampleRate;
        c.block_size        = lanes_test::kBufLength;
        c.input_channels    = lanes_test::kInChannels;
        c.output_channels   = lanes_test::kOutChannels;
        c.max_render_frames = 2048;
        c.time_origin_ntp   = kOrigin;
        ClockworkStatus st = CLOCKWORK_OK;
        h = clockwork_embed_attach(&c, &st);
        REQUIRE(h != nullptr);
        rate = clockwork_embed_sample_rate(h);
        if (in || out) REQUIRE(clockwork_embed_midi_ports(h, in, out) == CLOCKWORK_OK);
    }
    ~Host() { clockwork_embed_close(h); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void send(const osc_test::Packet& p) {
        REQUIRE(clockwork_client_send(clockwork_embed_client(h), p.ptr(), p.size(), 1) == CLOCKWORK_OK);
    }
    // A send for the time of `frame` in this host's count, as a client asks
    // the engine to hold one of its own verbs: "/clockwork/schedule <time>
    // <the message>". (A bundle is the guest's to hold, where the guest keeps
    // a schedule of its own.)
    void sendAt(uint64_t frame, const osc_test::Packet& p) {
        const double ntp = kOrigin + static_cast<double>(frame) / rate;
        osc_test::Builder b;
        b.begin("/clockwork/schedule")
            << osc::TimeTag(clockwork::Timetag::fromNtpSeconds(ntp).ntp())
            << osc::Blob(p.ptr(), static_cast<osc::osc_bundle_element_size_t>(p.size()));
        send(b.end());
    }
    // Everything the client was sent since it was last asked.
    std::vector<osc_test::ParsedReply> heard() {
        std::vector<osc_test::ParsedReply> all;
        ClockworkClientMessage m[64];
        for (uint32_t n; (n = clockwork_client_poll(clockwork_embed_client(h), m, 64)) > 0;)
            for (uint32_t k = 0; k < n; ++k) all.push_back(osc_test::parseReply(m[k].bytes, m[k].length));
        return all;
    }
    // What the host's port was sent in a render of `n` frames, each with its
    // frame in this host's count.
    struct Sent { uint64_t frame; std::vector<uint8_t> bytes; };
    std::vector<Sent> render(uint32_t n) {
        std::vector<float> l(n), r(n);
        float* out[2] = { l.data(), r.data() };
        REQUIRE(clockwork_embed_render(h, out, 2, nullptr, 0, n) == n);
        std::vector<Sent> sent;
        ClockworkEmbedMidi events[8];
        for (uint32_t got; (got = clockwork_embed_midi_out(h, events, 8)) > 0;)
            for (uint32_t i = 0; i < got; ++i) {
                REQUIRE(events[i].frame < n);
                REQUIRE(events[i].length <= 3);
                sent.push_back({ frames + events[i].frame,
                                 std::vector<uint8_t>(events[i].bytes, events[i].bytes + events[i].length) });
            }
        frames += n;
        return sent;
    }
    std::vector<Sent> renderAll(std::initializer_list<uint32_t> sizes) {
        std::vector<Sent> all;
        for (uint32_t n : sizes)
            for (auto& s : render(n)) all.push_back(std::move(s));
        return all;
    }
    ClockworkEmbedMidiStats stats() {
        ClockworkEmbedMidiStats s {};
        s.struct_bytes = sizeof s;
        REQUIRE(clockwork_embed_midi_stats(h, &s) == CLOCKWORK_OK);
        return s;
    }
    void wantMidi() {
        send(osc_test::message("/clockwork/midi/notify/subscribe"));
        render(256);
        heard();
    }
};

osc_test::Packet midiOut(const char* kind, const char* port, std::initializer_list<int32_t> values) {
    osc_test::Builder b;
    auto& s = b.begin((std::string("/clockwork/midi/out/") + kind).c_str());
    s << port;
    for (int32_t v : values) s << v;
    return b.end();
}

// The trailing timetag of an event, as NTP seconds.
double arrivalTime(const osc_test::ParsedReply& r) {
    REQUIRE(r.raw.size() >= 8);
    uint64_t t = 0;
    for (size_t i = r.raw.size() - 8; i < r.raw.size(); ++i) t = (t << 8) | r.raw[i];
    return clockwork::Timetag::fromNtp(t).ntpSeconds();
}

const uint8_t kNoteOn[3]  = { 0x90, 60, 100 };
const uint8_t kNoteOff[3] = { 0x80, 60, 0 };
}

// ── In ───────────────────────────────────────────────────────────────────────

TEST_CASE("embed midi: a host's note reaches the client as any port's note does, at the time of its frame",
          "[embed][midi]") {
    // Buffers that divide the engine's block, that do not, and that span several.
    for (uint32_t buffer : { 64u, 100u, 128u, 441u, 1000u }) {
        for (uint32_t frame : { 0u, 1u, 37u, buffer - 1u }) {
            INFO("a buffer of " << buffer << ", the note at its frame " << frame);
            Host host;
            host.wantMidi();
            host.render(buffer);
            const uint64_t at = host.frames + frame;
            REQUIRE(clockwork_embed_midi_in(host.h, kNoteOn, 3, frame) == CLOCKWORK_OK);
            host.render(buffer);
            host.render(buffer);
            int notes = 0;
            for (const auto& r : host.heard()) {
                if (r.address != "/clockwork/midi/in/note_on") continue;
                ++notes;
                CHECK(r.argString(0) == "daw");
                CHECK(r.argInt(1) == 1);
                CHECK(r.argInt(2) == 60);
                CHECK(r.argInt(3) == 100);
                const double want = kOrigin + static_cast<double>(at) / host.rate;
                CHECK(std::abs(arrivalTime(r) - want) < 0.5 / host.rate);
            }
            CHECK(notes == 1);
        }
    }
}

TEST_CASE("embed midi: several notes in one buffer arrive in the order of their frames, each with its own time",
          "[embed][midi]") {
    Host host;
    host.wantMidi();
    const uint64_t base = host.frames;
    const uint32_t at[] = { 3, 200, 200, 511 };
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t note[3] = { 0x92, static_cast<uint8_t>(60 + i), 90 };
        REQUIRE(clockwork_embed_midi_in(host.h, note, 3, at[i]) == CLOCKWORK_OK);
    }
    REQUIRE(clockwork_embed_midi_in(host.h, kNoteOff, 3, 511) == CLOCKWORK_OK);
    host.render(512);
    host.render(512);
    std::vector<std::pair<int, double>> notes;
    int offs = 0;
    for (const auto& r : host.heard()) {
        if (r.address == "/clockwork/midi/in/note_on") { CHECK(r.argInt(1) == 3); notes.emplace_back(r.argInt(2), arrivalTime(r)); }
        if (r.address == "/clockwork/midi/in/note_off") ++offs;
    }
    REQUIRE(notes.size() == 4);
    CHECK(offs == 1);
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(notes[i].first == static_cast<int>(60 + i));
        CHECK(std::abs(notes[i].second - (kOrigin + static_cast<double>(base + at[i]) / host.rate)) < 0.5 / host.rate);
    }
}

TEST_CASE("embed midi: a host's note is said only to a client that asked for MIDI", "[embed][midi]") {
    Host host;
    host.render(256);
    host.heard();
    REQUIRE(clockwork_embed_midi_in(host.h, kNoteOn, 3, 0) == CLOCKWORK_OK);
    host.render(512);
    for (const auto& r : host.heard()) CHECK(r.address != "/clockwork/midi/in/note_on");
}

TEST_CASE("embed midi: what is not one short message, or comes from a host with no port, is refused", "[embed][midi]") {
    Host host;
    const uint8_t sysex[4] = { 0xF0, 1, 2, 0xF7 };
    const uint8_t data[2]  = { 60, 100 };
    CHECK(clockwork_embed_midi_in(host.h, sysex, 4, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_in(host.h, data, 2, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_in(host.h, kNoteOn, 2, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_in(host.h, nullptr, 3, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_in(host.h, kNoteOn, 0, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_in(nullptr, kNoteOn, 3, 0) == CLOCKWORK_E_ARG);

    Host none(nullptr, nullptr);
    CHECK(clockwork_embed_midi_in(none.h, kNoteOn, 3, 0) == CLOCKWORK_E_ARG);
    Host outOnly(nullptr, "daw");
    CHECK(clockwork_embed_midi_in(outOnly.h, kNoteOn, 3, 0) == CLOCKWORK_E_ARG);

    // A name is one a port could have.
    CHECK(clockwork_embed_midi_ports(none.h, "", nullptr) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_ports(none.h, std::string(200, 'x').c_str(), nullptr) == CLOCKWORK_E_ARG);
    CHECK(clockwork_embed_midi_ports(nullptr, "daw", "daw") == CLOCKWORK_E_ARG);
}

TEST_CASE("embed midi: more notes than a render's store holds are refused and counted, and the rest arrive",
          "[embed][midi]") {
    Host host;
    host.wantMidi();
    uint32_t taken = 0, refused = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
        const ClockworkStatus st = clockwork_embed_midi_in(host.h, kNoteOn, 3, i % 512);
        if (st == CLOCKWORK_OK) ++taken;
        else { CHECK(st == CLOCKWORK_E_FULL); ++refused; }
    }
    CHECK(taken >= 256u);
    CHECK(refused > 0u);
    CHECK(host.stats().in_dropped == refused);
    uint32_t heard = 0;
    for (int i = 0; i < 4; ++i) {
        host.render(512);
        for (const auto& r : host.heard()) heard += r.address == "/clockwork/midi/in/note_on";
    }
    CHECK(heard == taken);
    // And the store is free again.
    CHECK(clockwork_embed_midi_in(host.h, kNoteOn, 3, 0) == CLOCKWORK_OK);
}

// ── Out ──────────────────────────────────────────────────────────────────────
//
// A send for a time is held by the engine's timed store. A build without one
// (CLOCKWORK_SCHEDULER=OFF) refuses "/clockwork/schedule" out loud, so these
// cases, which place their sends at frames, are for a build that has it; what
// a build without it does is pinned below them.

#if CLOCKWORK_SCHEDULER

TEST_CASE("embed midi: a send for a time comes out in the render that holds it, at its frame", "[embed][midi]") {
    const std::initializer_list<uint32_t> buffers[] = {
        { 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64 },
        { 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100 },
        { 512, 512, 512, 512 },
        { 441, 441, 441, 441, 441 },
        { 1, 7, 1000, 3, 1024, 13 },
        { 2048 },
    };
    for (const auto& sizes : buffers) {
        for (uint64_t frame : { 0ull, 1ull, 63ull, 64ull, 100ull, 777ull, 1500ull }) {
            INFO("buffers starting " << *sizes.begin() << ", the send for frame " << frame);
            Host host;
            const uint64_t at = host.frames + frame;
            host.sendAt(at, midiOut("note_on", "daw", { 1, 60, 100 }));
            const auto sent = host.renderAll(sizes);
            REQUIRE(sent.size() == 1);
            CHECK(sent[0].frame == at);
            CHECK(sent[0].bytes == std::vector<uint8_t>{ 0x90, 60, 100 });
            CHECK(host.stats().out_late == 0u);
            CHECK(host.stats().out_dropped == 0u);
        }
    }
}

TEST_CASE("embed midi: sends come out in the order of their times, a note's end after its start", "[embed][midi]") {
    Host host;
    host.render(256);
    const uint64_t base = host.frames;
    // Sent out of order, and two for one frame.
    host.sendAt(base + 900, midiOut("note_off", "daw", { 2, 64, 0 }));
    host.sendAt(base + 300, midiOut("note_on", "daw", { 2, 64, 80 }));
    host.sendAt(base + 300, midiOut("control_change", "daw", { 2, 7, 127 }));
    host.sendAt(base + 301, midiOut("pitch_bend", "daw", { 2, 8192 }));
    host.sendAt(base + 301, midiOut("program_change", "daw", { 16, 5 }));
    const auto sent = host.renderAll({ 512, 512, 512 });
    REQUIRE(sent.size() == 5);
    CHECK(sent[0].frame == base + 300);
    CHECK(sent[1].frame == base + 300);
    CHECK(sent[2].frame == base + 301);
    CHECK(sent[3].frame == base + 301);
    CHECK(sent[4].frame == base + 900);
    CHECK(sent[0].bytes == std::vector<uint8_t>{ 0x91, 64, 80 });
    CHECK(sent[1].bytes == std::vector<uint8_t>{ 0xB1, 7, 127 });
    CHECK(sent[2].bytes == std::vector<uint8_t>{ 0xE1, 0, 64 });
    CHECK(sent[3].bytes == std::vector<uint8_t>{ 0xCF, 5 });
    CHECK(sent[4].bytes == std::vector<uint8_t>{ 0x81, 64, 0 });
}

TEST_CASE("embed midi: a send with no time comes out at once and is not late; one whose time has passed is late",
          "[embed][midi]") {
    Host host;
    host.render(512);
    host.send(midiOut("note_on", "daw", { 1, 60, 100 }));
    auto sent = host.render(512);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].frame < host.frames);
    CHECK(host.stats().out_late == 0u);

    // A second of audio ago.
    host.render(static_cast<uint32_t>(host.rate));
    const uint64_t before = host.frames;
    host.sendAt(0, midiOut("note_off", "daw", { 1, 60, 0 }));
    sent = host.render(512);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].frame >= before);
    CHECK(sent[0].bytes == std::vector<uint8_t>{ 0x80, 60, 0 });
    CHECK(host.stats().out_late == 1u);
}

TEST_CASE("embed midi: a send to every channel is the message on each of the 16", "[embed][midi]") {
    Host host;
    host.sendAt(host.frames + 100, midiOut("note_on", "daw", { -1, 60, 100 }));
    const auto sent = host.renderAll({ 512 });
    REQUIRE(sent.size() == 16);
    for (uint8_t c = 0; c < 16; ++c) {
        CHECK(sent[c].frame == 100);
        CHECK(sent[c].bytes == std::vector<uint8_t>{ static_cast<uint8_t>(0x90 | c), 60, 100 });
    }
}

TEST_CASE("embed midi: a send to every port reaches the host's, and one to another port does not", "[embed][midi]") {
    Host host;
    host.sendAt(host.frames + 10, midiOut("note_on", "*", { 1, 61, 100 }));
    host.sendAt(host.frames + 20, midiOut("note_on", "some_other_port", { 1, 62, 100 }));
    host.sendAt(host.frames + 30, midiOut("note_on", "da", { 1, 63, 100 }));
    host.sendAt(host.frames + 40, midiOut("note_on", "daw2", { 1, 64, 100 }));
    const auto sent = host.renderAll({ 512 });
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].bytes == std::vector<uint8_t>{ 0x90, 61, 100 });
}

TEST_CASE("embed midi: a host with no port out is sent nothing", "[embed][midi]") {
    Host none(nullptr, nullptr);
    none.sendAt(none.frames + 10, midiOut("note_on", "*", { 1, 60, 100 }));
    CHECK(none.renderAll({ 512 }).empty());
    Host inOnly("daw", nullptr);
    inOnly.sendAt(inOnly.frames + 10, midiOut("note_on", "daw", { 1, 60, 100 }));
    CHECK(inOnly.renderAll({ 512 }).empty());
}

TEST_CASE("embed midi: more sends than a render's store holds are dropped and counted, and the rest come out",
          "[embed][midi]") {
    Host host;
    const uint32_t n = 400;
    // In several sends: one bundle of them all is more than a message may be.
    const uint64_t due = 4000;
    for (uint32_t i = 0; i < n; ++i) {
        host.sendAt(due, midiOut("note_on", "daw", { 1, static_cast<int32_t>(i % 128), 100 }));
        if (i % 16 == 15) host.render(64);
    }
    REQUIRE(host.frames < due);
    size_t out = 0;
    while (host.frames < 6000)
        for (const auto& s : host.render(512)) { CHECK(s.frame == due); ++out; }
    const auto stats = host.stats();
    CHECK(stats.out_dropped > 0u);
    CHECK(out + stats.out_dropped == n);
    // And the store is free again.
    host.sendAt(host.frames + 5, midiOut("note_on", "daw", { 1, 60, 100 }));
    CHECK(host.renderAll({ 512 }).size() == 1);
}

TEST_CASE("embed midi: what the host is not asked for stays for it, and a small array takes it in parts", "[embed][midi]") {
    Host host;
    for (int i = 0; i < 5; ++i) host.sendAt(host.frames + 50, midiOut("note_on", "daw", { 1, 60 + i, 100 }));
    std::vector<float> l(512), r(512);
    float* out[2] = { l.data(), r.data() };
    REQUIRE(clockwork_embed_render(host.h, out, 2, nullptr, 0, 512) == 512);
    ClockworkEmbedMidi events[2];
    CHECK(clockwork_embed_midi_out(host.h, events, 2) == 2);
    CHECK(events[0].bytes[1] == 60);
    CHECK(clockwork_embed_midi_out(host.h, events, 2) == 2);
    CHECK(events[0].bytes[1] == 62);
    CHECK(clockwork_embed_midi_out(host.h, events, 2) == 1);
    CHECK(events[0].bytes[1] == 64);
    CHECK(clockwork_embed_midi_out(host.h, events, 2) == 0);
    CHECK(clockwork_embed_midi_out(host.h, nullptr, 2) == 0);
    CHECK(clockwork_embed_midi_out(nullptr, events, 2) == 0);
}

TEST_CASE("embed midi: the core hands a tick's sends over once", "[embed][midi]") {
    Host host;
    host.sendAt(host.frames + 10, midiOut("note_on", "daw", { 1, 60, 100 }));
    ClockworkCore* core = clockwork_client_core(clockwork_embed_client(host.h));
    // Through the embed, which takes them after each tick: one, and the core then has none to hand over again.
    CHECK(host.renderAll({ 64 }).size() == 1);
    const ClockworkHostMidi* events = nullptr;
    CHECK(clockwork_host_midi_out(core, &events) == 0u);
    CHECK(clockwork_host_midi_out(core, nullptr) == 0u);
    CHECK(clockwork_host_midi_out(nullptr, &events) == 0u);
    CHECK(host.renderAll({ 512, 512 }).empty());
}

#else // no timed store

TEST_CASE("embed midi, no timed store: a send with no time still reaches the host's port, at once", "[embed][midi]") {
    Host host;
    host.render(512);
    host.send(midiOut("note_on", "daw", { 1, 60, 100 }));
    const auto sent = host.render(512);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].bytes == std::vector<uint8_t>{ 0x90, 60, 100 });
    CHECK(host.stats().out_late == 0u);
    CHECK(host.stats().out_dropped == 0u);
}

TEST_CASE("embed midi, no timed store: a send for a time is refused to its client, and nothing reaches the host",
          "[embed][midi]") {
    Host host;
    host.render(512);
    host.heard();
    host.sendAt(host.frames + 100, midiOut("note_on", "daw", { 1, 60, 100 }));
    // Not sent at once in place of its time, and not kept: the client asked for a time and is told it has none.
    CHECK(host.renderAll({ 512, 512 }).empty());
    int refused = 0;
    for (const auto& r : host.heard())
        refused += r.address == "/clockwork/error" && r.argString(0) == "/clockwork/schedule";
    CHECK(refused == 1);
    CHECK(host.stats().out_late == 0u);
    CHECK(host.stats().out_dropped == 0u);
}

#endif // CLOCKWORK_SCHEDULER

// ── The port list ────────────────────────────────────────────────────────────

TEST_CASE("embed midi: a host's ports are in the port list", "[embed][midi]") {
    Host host("keys", "daw");
    host.render(256);
    host.heard();
    host.send(osc_test::message("/clockwork/midi/ports/list"));
    host.render(256);
    int replies = 0;
    for (const auto& r : host.heard()) {
        if (r.address != "/clockwork/midi/ports.reply") continue;
        ++replies;
        REQUIRE(r.argCount() == 6);
        CHECK(r.argInt(0) == 1);
        CHECK(r.argString(1) == "keys");
        CHECK(r.argInt(2) == 1);
        CHECK(r.argInt(3) == 1);
        CHECK(r.argString(4) == "daw");
        CHECK(r.argInt(5) == 1);
    }
    CHECK(replies == 1);

    // A host with none answers as it did: it has no port list of its own.
    Host none(nullptr, nullptr);
    none.render(256);
    none.heard();
    none.send(osc_test::message("/clockwork/midi/ports/list"));
    none.render(256);
    for (const auto& r : none.heard()) CHECK(r.address != "/clockwork/midi/ports.reply");
}

TEST_CASE("embed midi: a client that asks for MIDI is told the host's ports, and is not refused", "[embed][midi]") {
    Host host("keys", "daw");
    host.render(256);
    host.heard();
    host.send(osc_test::message("/clockwork/midi/notify/subscribe"));
    host.render(256);
    int notices = 0;
    for (const auto& r : host.heard()) {
        CHECK(r.address != "/clockwork/error");
        CHECK(r.address != "/clockwork/refused");
        if (r.address != "/clockwork/midi/ports") continue;
        ++notices;
        REQUIRE(r.argCount() == 6);
        CHECK(r.argString(1) == "keys");
        CHECK(r.argString(4) == "daw");
    }
    CHECK(notices == 1);
    host.send(osc_test::message("/clockwork/midi/notify/unsubscribe"));
    host.render(256);
    for (const auto& r : host.heard()) {
        CHECK(r.address != "/clockwork/error");
        CHECK(r.address != "/clockwork/refused");
        CHECK(r.address != "/clockwork/midi/ports");
    }
    REQUIRE(clockwork_embed_midi_in(host.h, kNoteOn, 3, 0) == CLOCKWORK_OK);
    host.render(256);
    for (const auto& r : host.heard()) CHECK(r.address != "/clockwork/midi/in/note_on");
}

TEST_CASE("embed midi: two engines keep two hosts' MIDI apart", "[embed][midi]") {
    Host a, b;
    a.wantMidi();
    b.wantMidi();
    REQUIRE(clockwork_embed_midi_in(a.h, kNoteOn, 3, 5) == CLOCKWORK_OK);
    b.send(midiOut("note_on", "daw", { 1, 72, 100 }));   // with no time: a build with no timed store sends it too
    const auto fromA = a.renderAll({ 512 });
    const auto fromB = b.renderAll({ 512 });
    CHECK(fromA.empty());
    REQUIRE(fromB.size() == 1);
    int heardA = 0, heardB = 0;
    for (const auto& r : a.heard()) heardA += r.address == "/clockwork/midi/in/note_on";
    for (const auto& r : b.heard()) heardB += r.address == "/clockwork/midi/in/note_on";
    CHECK(heardA == 1);
    CHECK(heardB == 0);
}
