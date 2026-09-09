// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_ports.cpp — the port substrate through its C ABI, plus the two disk
 * endpoints and the boundary they meet the DSP on.
 *
 * The ring's own arithmetic is pinned in Rust (rust/clockwork-ports —
 * src/ring.rs unit tests, tests/concurrency.rs, tests/no_alloc.rs). What is
 * pinned HERE is everything a C caller can see and everything clockwork
 * actually does with it: the ABI shape, the slot rules, the counters, the
 * endpoints, and audio arriving at and leaving the DSP.
 *
 * Two of these cases are worth more than the rest.
 *
 *   - The CONCURRENCY case runs a real producer thread against a real consumer
 *     loop for half a million frames and reconciles the counters exactly.
 *     Every other case here would pass against a ring with a torn wrap.
 *   - The BOUNDARY cases drive audio in through a port, through dsp_process, and
 *     back out through another port. Everything else tests the substrate; only
 *     these test that it is connected to anything.
 *
 * Ports are a process-global table, so every case closes what it opened. The
 * `Ports` guard does it even on a failed REQUIRE, because a leaked slot would
 * make the next case's slot numbering — the thing under test — a lie.
 */
#include "clockwork_ports.h"
#include "clockwork_port_bus.h"
#include "clockwork_disk.h"

#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>

#include "clockwork_audio_file.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

// Everything this file opens goes away at the end of the case, pass or fail.
struct Ports {
    ~Ports() { clockwork_port_bus_detach_all(); clockwork_port_close_all(); }
};

// Frame f, channel c. Integers below 2^24 are exact in float, so every
// assertion below is an equality and not a tolerance.
float sample(uint64_t f, uint32_t c) { return static_cast<float>(f * 4 + c); }

std::filesystem::path tempDir(const char* leaf) {
    auto dir = std::filesystem::temp_directory_path() / "clockwork_ports_test" / leaf;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// A planar block, and the pointer array the ABI wants over it.
struct Block {
    std::vector<std::vector<float>> ch;
    std::vector<float*>       out;
    std::vector<const float*> in;
    Block(uint32_t channels, uint32_t frames, float fill = 0.0f)
        : ch(channels, std::vector<float>(frames, fill)) {
        for (auto& v : ch) { out.push_back(v.data()); in.push_back(v.data()); }
    }
};

} // namespace

// ── The round trip ──────────────────────────────────────────────────────────

TEST_CASE("ports: a source round trip is sample for sample", "[ports]") {
    Ports guard;
    const ClockworkPort p = clockwork_port_open("rt-source", kClockworkPortSource, 2, 1000);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_is_open(p) == 1);
    REQUIRE(clockwork_port_channels(p) == 2);
    REQUIRE(clockwork_port_direction(p) == kClockworkPortSource);
    REQUIRE(std::string(clockwork_port_name(p)) == "rt-source");
    // Capacity is rounded UP to a power of two, so 1000 buys 1024.
    REQUIRE(clockwork_port_writable(p) == 1024);
    REQUIRE(clockwork_port_readable(p) == 0);

    std::vector<float> interleaved(64 * 2);
    for (uint32_t f = 0; f < 64; ++f)
        for (uint32_t c = 0; c < 2; ++c) interleaved[f * 2 + c] = sample(f, c);
    REQUIRE(clockwork_port_produce(p, interleaved.data(), 64) == 64);
    REQUIRE(clockwork_port_readable(p) == 64);
    REQUIRE(clockwork_port_writable(p) == 1024 - 64);

    Block b(2, 64, -1.0f);
    REQUIRE(clockwork_port_read(p, b.out.data(), 2, 64) == 64);
    for (uint32_t f = 0; f < 64; ++f)
        for (uint32_t c = 0; c < 2; ++c) REQUIRE(b.ch[c][f] == sample(f, c));
    REQUIRE(clockwork_port_underruns(p) == 0);
    REQUIRE(clockwork_port_overruns(p) == 0);
}

TEST_CASE("ports: a sink round trip is sample for sample", "[ports]") {
    Ports guard;
    const ClockworkPort p = clockwork_port_open("rt-sink", kClockworkPortSink, 3, 256);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_direction(p) == kClockworkPortSink);

    Block b(3, 32);
    for (uint32_t f = 0; f < 32; ++f)
        for (uint32_t c = 0; c < 3; ++c) b.ch[c][f] = sample(f, c);
    REQUIRE(clockwork_port_write(p, b.in.data(), 3, 32) == 32);
    REQUIRE(clockwork_port_readable(p) == 32);

    std::vector<float> out(32 * 3, -1.0f);
    REQUIRE(clockwork_port_consume(p, out.data(), 32) == 32);
    for (uint32_t f = 0; f < 32; ++f)
        for (uint32_t c = 0; c < 3; ++c) REQUIRE(out[f * 3 + c] == sample(f, c));
    REQUIRE(clockwork_port_overruns(p) == 0);
}

// ── The two policies ────────────────────────────────────────────────────────

TEST_CASE("ports: an empty source reads silence and counts the shortfall",
          "[ports]") {
    Ports guard;
    const ClockworkPort p = clockwork_port_open("underrun", kClockworkPortSource, 2, 128);
    REQUIRE(p != CLOCKWORK_PORT_NONE);

    // Nothing produced at all: a whole block of silence, and the counter says
    // exactly how much.
    Block b(2, 64, 7.0f);
    REQUIRE(clockwork_port_read(p, b.out.data(), 2, 64) == 0);
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t f = 0; f < 64; ++f) REQUIRE(b.ch[c][f] == 0.0f);
    REQUIRE(clockwork_port_underruns(p) == 64);

    // A PARTIAL block: the frames that were there, then silence, and the
    // counter moves by the shortfall — not by a whole block, which would make
    // the number useless for telling "nearly keeping up" from "dead".
    std::vector<float> some(20 * 2);
    for (uint32_t f = 0; f < 20; ++f)
        for (uint32_t c = 0; c < 2; ++c) some[f * 2 + c] = sample(f, c);
    REQUIRE(clockwork_port_produce(p, some.data(), 20) == 20);

    Block b2(2, 64, 7.0f);
    REQUIRE(clockwork_port_read(p, b2.out.data(), 2, 64) == 20);
    for (uint32_t c = 0; c < 2; ++c) {
        for (uint32_t f = 0; f < 20; ++f) REQUIRE(b2.ch[c][f] == sample(f, c));
        for (uint32_t f = 20; f < 64; ++f) REQUIRE(b2.ch[c][f] == 0.0f);
    }
    REQUIRE(clockwork_port_underruns(p) == 64 + 44);

    // And the audio path keeps running: an underrun is a fact, not a state to
    // recover from. Feed it again and it delivers again.
    REQUIRE(clockwork_port_produce(p, some.data(), 20) == 20);
    Block b3(2, 20, 7.0f);
    REQUIRE(clockwork_port_read(p, b3.out.data(), 2, 20) == 20);
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t f = 0; f < 20; ++f) REQUIRE(b3.ch[c][f] == sample(f, c));
    REQUIRE(clockwork_port_underruns(p) == 64 + 44);   // unchanged: nothing was short
}

TEST_CASE("ports: a full sink drops, counts, and does not block", "[ports]") {
    Ports guard;
    // 16 frames deep — the floor, and the smallest ring that exists.
    const ClockworkPort p = clockwork_port_open("overrun", kClockworkPortSink, 1, 1);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_writable(p) == 16);

    Block b(1, 16);
    for (uint32_t f = 0; f < 16; ++f) b.ch[0][f] = sample(f, 0);
    REQUIRE(clockwork_port_write(p, b.in.data(), 1, 16) == 16);
    REQUIRE(clockwork_port_writable(p) == 0);
    REQUIRE(clockwork_port_overruns(p) == 0);

    // Full. The next block is dropped WHOLE and counted whole, and the call
    // returns rather than waiting for the endpoint.
    const auto before = std::chrono::steady_clock::now();
    REQUIRE(clockwork_port_write(p, b.in.data(), 1, 16) == 0);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    REQUIRE(clockwork_port_overruns(p) == 16);
    REQUIRE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 50);

    // A PARTIAL drop: make room for 4, offer 16, and 12 are counted.
    std::vector<float> drained(4, -1.0f);
    REQUIRE(clockwork_port_consume(p, drained.data(), 4) == 4);
    REQUIRE(clockwork_port_write(p, b.in.data(), 1, 16) == 4);
    REQUIRE(clockwork_port_overruns(p) == 16 + 12);

    // What survived is still the right samples: dropping does not corrupt what
    // was accepted.
    std::vector<float> rest(16, -1.0f);
    REQUIRE(clockwork_port_consume(p, rest.data(), 16) == 16);
    for (uint32_t f = 0; f < 12; ++f) REQUIRE(rest[f] == sample(f + 4, 0));
    for (uint32_t f = 12; f < 16; ++f) REQUIRE(rest[f] == sample(f - 12, 0));
}

// ── Mismatch, which is handled rather than refused ──────────────────────────

TEST_CASE("ports: a channel-count mismatch is handled, not refused", "[ports]") {
    Ports guard;
    // The device can change under a running stream, so neither direction of
    // mismatch may be an error.
    const ClockworkPort mono = clockwork_port_open("mono-source", kClockworkPortSource, 1, 64);
    REQUIRE(mono != CLOCKWORK_PORT_NONE);
    std::vector<float> one(8);
    for (uint32_t f = 0; f < 8; ++f) one[f] = sample(f, 0);
    REQUIRE(clockwork_port_produce(mono, one.data(), 8) == 8);

    // A caller with MORE channels than the port: the extras are silence.
    Block wide(4, 8, 9.0f);
    REQUIRE(clockwork_port_read(mono, wide.out.data(), 4, 8) == 8);
    for (uint32_t f = 0; f < 8; ++f) {
        REQUIRE(wide.ch[0][f] == sample(f, 0));
        for (uint32_t c = 1; c < 4; ++c) REQUIRE(wide.ch[c][f] == 0.0f);
    }

    // A caller with FEWER channels than the port: the extras are discarded.
    const ClockworkPort quad = clockwork_port_open("quad-source", kClockworkPortSource, 4, 64);
    REQUIRE(quad != CLOCKWORK_PORT_NONE);
    std::vector<float> four(8 * 4);
    for (uint32_t f = 0; f < 8; ++f)
        for (uint32_t c = 0; c < 4; ++c) four[f * 4 + c] = sample(f, c);
    REQUIRE(clockwork_port_produce(quad, four.data(), 8) == 8);
    Block narrow(2, 8, 9.0f);
    REQUIRE(clockwork_port_read(quad, narrow.out.data(), 2, 8) == 8);
    for (uint32_t f = 0; f < 8; ++f)
        for (uint32_t c = 0; c < 2; ++c) REQUIRE(narrow.ch[c][f] == sample(f, c));

    // The same both ways on a sink: an unoffered port channel is silence in
    // the ring, so a frame the endpoint sees is always whole.
    const ClockworkPort sink = clockwork_port_open("wide-sink", kClockworkPortSink, 3, 64);
    REQUIRE(sink != CLOCKWORK_PORT_NONE);
    Block two(2, 8);
    for (uint32_t f = 0; f < 8; ++f)
        for (uint32_t c = 0; c < 2; ++c) two.ch[c][f] = sample(f, c);
    REQUIRE(clockwork_port_write(sink, two.in.data(), 2, 8) == 8);
    std::vector<float> got(8 * 3, -1.0f);
    REQUIRE(clockwork_port_consume(sink, got.data(), 8) == 8);
    for (uint32_t f = 0; f < 8; ++f) {
        REQUIRE(got[f * 3 + 0] == sample(f, 0));
        REQUIRE(got[f * 3 + 1] == sample(f, 1));
        REQUIRE(got[f * 3 + 2] == 0.0f);
    }
}

// ── Slots ───────────────────────────────────────────────────────────────────

TEST_CASE("ports: closing one does not renumber the others", "[ports]") {
    Ports guard;
    clockwork_port_close_all();

    const ClockworkPort a = clockwork_port_open("a", kClockworkPortSource, 1, 64);
    const ClockworkPort b = clockwork_port_open("b", kClockworkPortSource, 1, 64);
    const ClockworkPort c = clockwork_port_open("c", kClockworkPortSource, 1, 64);
    REQUIRE(a != CLOCKWORK_PORT_NONE);
    REQUIRE(b != CLOCKWORK_PORT_NONE);
    REQUIRE(c != CLOCKWORK_PORT_NONE);
    REQUIRE(a != b);
    REQUIRE(b != c);

    // Give each a signal that names it, and leave it unread.
    for (auto [port, tag] : {std::pair<ClockworkPort, float>{a, 100.0f},
                             std::pair<ClockworkPort, float>{b, 200.0f},
                             std::pair<ClockworkPort, float>{c, 300.0f}}) {
        std::vector<float> v(8);
        for (uint32_t f = 0; f < 8; ++f) v[f] = tag + static_cast<float>(f);
        REQUIRE(clockwork_port_produce(port, v.data(), 8) == 8);
    }

    ClockworkPort listed[8] = {};
    REQUIRE(clockwork_port_list(listed, 8) == 3);

    // Close the MIDDLE one. Renumbering here would hand a reader somebody
    // else's audio on a channel it is already processing.
    clockwork_port_close(b);
    REQUIRE(clockwork_port_is_open(b) == 0);
    REQUIRE(clockwork_port_is_open(a) == 1);
    REQUIRE(clockwork_port_is_open(c) == 1);
    REQUIRE(clockwork_port_list(listed, 8) == 2);
    REQUIRE(listed[0] == a);
    REQUIRE(listed[1] == c);

    // The handles are unchanged AND still address their own audio.
    Block ba(1, 8, -1.0f), bc(1, 8, -1.0f);
    REQUIRE(clockwork_port_read(a, ba.out.data(), 1, 8) == 8);
    REQUIRE(clockwork_port_read(c, bc.out.data(), 1, 8) == 8);
    for (uint32_t f = 0; f < 8; ++f) {
        REQUIRE(ba.ch[0][f] == 100.0f + static_cast<float>(f));
        REQUIRE(bc.ch[0][f] == 300.0f + static_cast<float>(f));
    }

    // A NEW port may take the freed slot — that is the point of freeing it —
    // but it must never be handed the closed port's handle, or a client still
    // holding `b` would start reading the newcomer's audio.
    const ClockworkPort d = clockwork_port_open("d", kClockworkPortSource, 1, 64);
    REQUIRE(d != CLOCKWORK_PORT_NONE);
    REQUIRE(d != b);
    REQUIRE(d != a);
    REQUIRE(d != c);
    REQUIRE(clockwork_port_is_open(b) == 0);

    // And `b` still reads silence rather than d's audio.
    std::vector<float> dv(8, 42.0f);
    REQUIRE(clockwork_port_produce(d, dv.data(), 8) == 8);
    Block bb(1, 8, -1.0f);
    REQUIRE(clockwork_port_read(b, bb.out.data(), 1, 8) == 0);
    for (uint32_t f = 0; f < 8; ++f) REQUIRE(bb.ch[0][f] == 0.0f);
}

TEST_CASE("ports: a closed, stale or nonsense handle reads silence, not a fault",
          "[ports]") {
    Ports guard;
    const ClockworkPort p = clockwork_port_open("doomed", kClockworkPortSource, 2, 64);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    std::vector<float> v(8 * 2, 5.0f);
    REQUIRE(clockwork_port_produce(p, v.data(), 8) == 8);

    clockwork_port_close(p);
    clockwork_port_close(p);   // twice is safe, says the header

    Block b(2, 8, 3.0f);
    REQUIRE(clockwork_port_read(p, b.out.data(), 2, 8) == 0);
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t f = 0; f < 8; ++f) REQUIRE(b.ch[c][f] == 0.0f);

    // Writes are discarded, not faults; queries answer emptily.
    REQUIRE(clockwork_port_write(p, b.in.data(), 2, 8) == 0);
    REQUIRE(clockwork_port_produce(p, v.data(), 8) == 0);
    REQUIRE(clockwork_port_consume(p, v.data(), 8) == 0);
    REQUIRE(clockwork_port_channels(p) == 0);
    REQUIRE(clockwork_port_direction(p) == 0);
    REQUIRE(clockwork_port_writable(p) == 0);
    REQUIRE(clockwork_port_readable(p) == 0);
    REQUIRE(clockwork_port_underruns(p) == 0);
    REQUIRE(clockwork_port_overruns(p) == 0);
    REQUIRE(clockwork_port_name(p) == nullptr);

    // Handles that were never valid take the same path.
    for (ClockworkPort bogus : {CLOCKWORK_PORT_NONE, ClockworkPort{0xdeadbeef}, ClockworkPort{0xffff}}) {
        Block z(2, 8, 3.0f);
        REQUIRE(clockwork_port_read(bogus, z.out.data(), 2, 8) == 0);
        for (uint32_t c = 0; c < 2; ++c)
            for (uint32_t f = 0; f < 8; ++f) REQUIRE(z.ch[c][f] == 0.0f);
        REQUIRE(clockwork_port_write(bogus, z.in.data(), 2, 8) == 0);
        clockwork_port_close(bogus);
    }
}

TEST_CASE("ports: open refuses only what cannot mean anything", "[ports]") {
    Ports guard;
    clockwork_port_close_all();

    REQUIRE(clockwork_port_open("bad-dir", static_cast<ClockworkPortDirection>(0), 2, 64)
            == CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_open("bad-dir", static_cast<ClockworkPortDirection>(7), 2, 64)
            == CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_open("no-channels", kClockworkPortSource, 0, 64) == CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_open("too-many-channels", kClockworkPortSource, 4096, 64)
            == CLOCKWORK_PORT_NONE);

    // A nonsense capacity is CLAMPED, not refused: 0 frames is a request that
    // means nothing, and the smallest useful ring is a better answer than a
    // failure the caller has to handle.
    const ClockworkPort tiny = clockwork_port_open("tiny", kClockworkPortSource, 1, 0);
    REQUIRE(tiny != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_writable(tiny) == 16);
    clockwork_port_close(tiny);

    // A NULL name is a name, not an error.
    const ClockworkPort anon = clockwork_port_open(nullptr, kClockworkPortSink, 1, 64);
    REQUIRE(anon != CLOCKWORK_PORT_NONE);
    REQUIRE(std::string(clockwork_port_name(anon)).empty());
    clockwork_port_close(anon);

    // The table is finite and says so rather than growing on the audio
    // thread's behalf.
    std::vector<ClockworkPort> all;
    while (true) {
        const ClockworkPort p = clockwork_port_open("filler", kClockworkPortSource, 1, 16);
        if (p == CLOCKWORK_PORT_NONE) break;
        all.push_back(p);
        REQUIRE(all.size() <= 4096);
    }
    REQUIRE(all.size() == 64);
    REQUIRE(clockwork_port_list(nullptr, 0) == 64);
    for (ClockworkPort p : all) clockwork_port_close(p);
    REQUIRE(clockwork_port_list(nullptr, 0) == 0);
    // Every slot comes back after the last reader leaves.
    REQUIRE(clockwork_port_open("after", kClockworkPortSource, 1, 16) != CLOCKWORK_PORT_NONE);
}

// ── The one that earns it ───────────────────────────────────────────────────

TEST_CASE("ports: a producer thread and an audio loop reconcile exactly",
          "[ports][concurrency]") {
    Ports guard;
    constexpr uint32_t kCh    = 3;
    constexpr uint64_t kTotal = 500000;
    // The PRODUCER's block deliberately does not divide the 1024-frame ring.
    // With 128 against 1024 the write cursor stays aligned and a push never
    // straddles the wrap, so the ring's hardest case goes unexercised for the
    // whole run — deleting the wrap-segment copy left this case green until
    // the block became 100. The consumer reads 128, which is what an audio
    // callback would do.
    constexpr uint32_t kProduce = 100;
    constexpr uint32_t kBlock   = 128;

    const ClockworkPort p = clockwork_port_open("hammered", kClockworkPortSource, kCh, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);

    // The endpoint. Keeps the remainder when the ring is full, which is what
    // clockwork_ports.h tells a producer to do and the only way "nothing is lost"
    // can be asserted at the end.
    std::atomic<uint64_t> produced{0};
    std::thread worker([&] {
        std::vector<float> buf(kProduce * kCh);
        uint64_t next = 0;
        while (next < kTotal) {
            const uint32_t n = static_cast<uint32_t>(
                std::min<uint64_t>(kProduce, kTotal - next));
            for (uint32_t f = 0; f < n; ++f)
                for (uint32_t c = 0; c < kCh; ++c)
                    buf[f * kCh + c] = sample(next + f, c);
            uint32_t done = 0;
            while (done < n) {
                const uint32_t took =
                    clockwork_port_produce(p, buf.data() + done * kCh, n - done);
                if (took == 0) std::this_thread::yield();
                done += took;
            }
            next += n;
            produced.store(next, std::memory_order_relaxed);
        }
    });

    // The audio thread: a fixed cadence, taking whatever is there, never
    // waiting for the endpoint.
    Block b(kCh, kBlock, -1.0f);
    uint64_t out = 0, requested = 0;
    uint64_t torn = 0, wrong = 0;
    while (out < kTotal) {
        const uint32_t got = clockwork_port_read(p, b.out.data(), kCh, kBlock);
        requested += kBlock;
        for (uint32_t f = 0; f < got; ++f)
            for (uint32_t c = 0; c < kCh; ++c)
                if (b.ch[c][f] != sample(out + f, c)) ++torn;
        // The zero-filled tail, every block, on every channel.
        for (uint32_t c = 0; c < kCh; ++c)
            for (uint32_t f = got; f < kBlock; ++f)
                if (b.ch[c][f] != 0.0f) ++wrong;
        out += got;
    }
    worker.join();

    // Counted rather than REQUIREd inside the loop: half a million REQUIREs is
    // a slow suite, and one assertion over the count says the same thing.
    REQUIRE(torn == 0);
    REQUIRE(wrong == 0);
    REQUIRE(produced.load() == kTotal);
    REQUIRE(out == kTotal);                      // nothing lost, nothing repeated
    REQUIRE(clockwork_port_overruns(p) == 0);          // a source never overruns
    REQUIRE(clockwork_port_underruns(p) == requested - out);   // frames in == out + silence
}

// ── The endpoints ───────────────────────────────────────────────────────────

TEST_CASE("ports: the disk sink and the disk source round trip a signal",
          "[ports][disk]") {
    Ports guard;
    const auto dir  = tempDir("wav_roundtrip");
    const auto path = (dir / "roundtrip.wav").string();
    constexpr uint32_t kCh = 2, kFrames = 5000, kBlock = 128;

    // Out: the audio thread writes blocks, a worker drains them to disk.
    {
        const ClockworkPort sink = clockwork_port_open("disk-out", kClockworkPortSink, kCh, 8192);
        REQUIRE(sink != CLOCKWORK_PORT_NONE);
        ClockworkDiskSink* w = clockwork_disk_sink_open(path.c_str(), sink, 48000, kCh);
        REQUIRE(w != nullptr);

        Block b(kCh, kBlock);
        for (uint32_t f = 0; f < kFrames; f += kBlock) {
            const uint32_t n = std::min<uint32_t>(kBlock, kFrames - f);
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t c = 0; c < kCh; ++c)
                    b.ch[c][i] = sample(f + i, c) / 32768.0f;
            // Wait for room rather than dropping: this is a test of the file,
            // not of the drop policy, which has its own case.
            while (clockwork_port_writable(sink) < n) std::this_thread::yield();
            REQUIRE(clockwork_port_write(sink, b.in.data(), kCh, n) == n);
        }
        clockwork_disk_sink_close(w);      // drains, patches the header, joins
        REQUIRE(clockwork_port_overruns(sink) == 0);
        clockwork_port_close(sink);
    }
    REQUIRE(std::filesystem::exists(path));

    // In: a worker reads the file, the audio thread reads the port.
    {
        const ClockworkPort source = clockwork_port_open("disk-in", kClockworkPortSource, kCh, 8192);
        REQUIRE(source != CLOCKWORK_PORT_NONE);
        ClockworkDiskSource* r = clockwork_disk_source_open(path.c_str(), source);
        REQUIRE(r != nullptr);
        REQUIRE(clockwork_disk_source_frames(r) == kFrames);
        REQUIRE(clockwork_disk_source_sample_rate(r) == 48000);

        Block b(kCh, kBlock, -1.0f);
        uint32_t out = 0, mismatches = 0;
        for (int spins = 0; out < kFrames && spins < 200000; ++spins) {
            const uint32_t got = clockwork_port_read(source, b.out.data(), kCh, kBlock);
            for (uint32_t i = 0; i < got; ++i)
                for (uint32_t c = 0; c < kCh; ++c)
                    if (b.ch[c][i] != sample(out + i, c) / 32768.0f) ++mismatches;
            out += got;
            if (got == 0) std::this_thread::yield();
        }
        REQUIRE(mismatches == 0);
        REQUIRE(out == kFrames);
        REQUIRE(clockwork_disk_source_eof(r) == 1);
        REQUIRE(clockwork_disk_source_frames_produced(r) == kFrames);
        clockwork_disk_source_close(r);
        clockwork_port_close(source);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("ports: the WAV the sink writes is the WAV another decoder reads",
          "[ports][disk]") {
    // The round trip above would pass against a private format that only this
    // code can read. An independent implementation is what makes it a WAV: the
    // sink's writer is hand-written Rust in rust/clockwork-ports-disk, and the
    // reader here is the vendored dr_wav behind clockwork_audio_file.h. They
    // share no line of code, so agreement is agreement about the format.
    Ports guard;
    const auto dir  = tempDir("wav_decode");
    const auto path = (dir / "decoded.wav").string();
    constexpr uint32_t kCh = 2, kFrames = 777;

    const ClockworkPort sink = clockwork_port_open("disk-out", kClockworkPortSink, kCh, 4096);
    REQUIRE(sink != CLOCKWORK_PORT_NONE);
    ClockworkDiskSink* w = clockwork_disk_sink_open(path.c_str(), sink, 44100, kCh);
    REQUIRE(w != nullptr);
    Block b(kCh, kFrames);
    for (uint32_t f = 0; f < kFrames; ++f)
        for (uint32_t c = 0; c < kCh; ++c) b.ch[c][f] = sample(f, c) / 65536.0f;
    REQUIRE(clockwork_port_write(sink, b.in.data(), kCh, kFrames) == kFrames);
    clockwork_disk_sink_close(w);
    clockwork_port_close(sink);

    ClockworkAudioInfo info{};
    info.struct_bytes = sizeof(info);
    float* got = nullptr;
    REQUIRE(clockwork_audio_decode_file(path.c_str(), &info, &got) == CLOCKWORK_OK);
    REQUIRE(got != nullptr);
    REQUIRE(info.channels == kCh);
    REQUIRE(info.sample_rate == 44100);
    REQUIRE(info.frames == kFrames);
    REQUIRE(info.format == CLOCKWORK_AUDIO_FORMAT_WAV);
    REQUIRE(info.encoding == CLOCKWORK_AUDIO_ENCODING_FLOAT32);
    for (uint32_t fr = 0; fr < kFrames; ++fr)
        for (uint32_t c = 0; c < kCh; ++c)
            REQUIRE(got[fr * kCh + c] == sample(fr, c) / 65536.0f);
    clockwork_audio_free(got);
    std::filesystem::remove_all(dir);
}

TEST_CASE("ports: the disk source reads 16-bit PCM written by another encoder",
          "[ports][disk]") {
    // The other half of the hand-written parser: a file this code did not
    // write, in the format it does not write. The file comes from dr_wav,
    // behind clockwork_audio_file.h, and is read by the Rust parser.
    Ports guard;
    const auto dir  = tempDir("wav_pcm16");
    const auto path = (dir / "pcm16.wav").string();
    constexpr uint32_t kCh = 1, kFrames = 600;

    ClockworkAudioWriterConfig cfg{};
    cfg.struct_bytes = sizeof(cfg);
    cfg.format       = CLOCKWORK_AUDIO_FORMAT_WAV;
    cfg.encoding     = CLOCKWORK_AUDIO_ENCODING_PCM_S16;
    cfg.channels     = kCh;
    cfg.sample_rate  = 22050;
    ClockworkStatus wst = CLOCKWORK_E_ARG;
    ClockworkAudioWriter* wr = clockwork_audio_writer_open(path.c_str(), &cfg, &wst);
    REQUIRE(wr != nullptr);
    std::vector<short> pcm(kFrames);
    std::vector<float> asFloat(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        pcm[i] = static_cast<short>(static_cast<int>(i) * 37 - 10000);
        asFloat[i] = static_cast<float>(pcm[i]) / 32768.0f;
    }
    REQUIRE(clockwork_audio_writer_write(wr, asFloat.data(), kFrames) == CLOCKWORK_OK);
    REQUIRE(clockwork_audio_writer_close(wr, nullptr, nullptr) == CLOCKWORK_OK);

    const ClockworkPort source = clockwork_port_open("pcm16", kClockworkPortSource, kCh, 2048);
    REQUIRE(source != CLOCKWORK_PORT_NONE);
    ClockworkDiskSource* r = clockwork_disk_source_open(path.c_str(), source);
    REQUIRE(r != nullptr);
    REQUIRE(clockwork_disk_source_frames(r) == kFrames);
    REQUIRE(clockwork_disk_source_sample_rate(r) == 22050);

    Block b(kCh, 256, -1.0f);
    uint32_t out = 0, mismatches = 0;
    for (int spins = 0; out < kFrames && spins < 200000; ++spins) {
        const uint32_t got = clockwork_port_read(source, b.out.data(), kCh, 256);
        for (uint32_t i = 0; i < got; ++i)
            if (b.ch[0][i] != static_cast<float>(pcm[out + i]) / 32768.0f) ++mismatches;
        out += got;
        if (got == 0) std::this_thread::yield();
    }
    REQUIRE(mismatches == 0);
    REQUIRE(out == kFrames);
    clockwork_disk_source_close(r);
    clockwork_port_close(source);
    std::filesystem::remove_all(dir);
}

TEST_CASE("ports: a disk endpoint refuses at open rather than going silent",
          "[ports][disk]") {
    Ports guard;
    const auto dir = tempDir("wav_refusals");
    const ClockworkPort source = clockwork_port_open("refuse", kClockworkPortSource, 2, 64);
    REQUIRE(source != CLOCKWORK_PORT_NONE);

    // No such file. A silence to be diagnosed later is the wrong answer.
    REQUIRE(clockwork_disk_source_open((dir / "absent.wav").string().c_str(), source)
            == nullptr);

    // A file that is not a WAV.
    const auto junk = (dir / "junk.wav").string();
    { std::vector<char> j(64, 'x'); FILE* fp = std::fopen(junk.c_str(), "wb");
      REQUIRE(fp != nullptr); std::fwrite(j.data(), 1, j.size(), fp); std::fclose(fp); }
    REQUIRE(clockwork_disk_source_open(junk.c_str(), source) == nullptr);

    // A closed port.
    clockwork_port_close(source);
    REQUIRE(clockwork_disk_source_open(junk.c_str(), source) == nullptr);
    REQUIRE(clockwork_disk_sink_open((dir / "out.wav").string().c_str(), source, 48000, 2)
            == nullptr);

    // A sink onto a directory that does not exist.
    const ClockworkPort sink = clockwork_port_open("refuse-sink", kClockworkPortSink, 2, 64);
    REQUIRE(sink != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_disk_sink_open((dir / "nope" / "out.wav").string().c_str(),
                               sink, 48000, 2) == nullptr);
    std::filesystem::remove_all(dir);
}

// ── The boundary ────────────────────────────────────────────────────────────────

TEST_CASE("ports: a source meets the DSP as input channels", "[ports][boundary][audio]") {
    // The claim docs/PORTS.md makes and nothing else here tests: audio
    // arriving from somewhere that is not the device reaches dsp_process on
    // ordinary input channels, and the DSP needs no concept of where it came
    // from. The placeholder's echo mode makes it observable — output channel c
    // is input channel c verbatim.
    Ports guard;
    const uint32_t bl = lanes_test::boot();
    REQUIRE(lanes_test::kInChannels >= 2);

    const auto echo = osc_test::message("/dummy/echo");
    REQUIRE(lanes_test::ingress(echo.ptr(), echo.size(), 0));
    lanes_test::tick();          // the block that applies it
    lanes_test::drainRt();

    // Whatever the device backend left in the input staging buffer; a bound
    // source OWNS its range and must overwrite it.
    float* in = clockwork_audio_in();
    REQUIRE(in != nullptr);
    for (uint32_t ch = 0; ch < lanes_test::kInChannels; ++ch)
        for (uint32_t i = 0; i < bl; ++i) in[ch * bl + i] = -5.0f;

    const ClockworkPort p = clockwork_port_open("boundary-source", kClockworkPortSource, 2, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    std::vector<float> v(bl * 2);
    for (uint32_t i = 0; i < bl; ++i)
        for (uint32_t c = 0; c < 2; ++c)
            v[i * 2 + c] = static_cast<float>(c + 1) + static_cast<float>(i) / 1000.0f;
    REQUIRE(clockwork_port_produce(p, v.data(), bl) == bl);
    REQUIRE(clockwork_port_bus_attach(p, 0) == 1);
    REQUIRE(clockwork_port_bus_attached() == 1);
    // Attaching twice is refused rather than silently reading the port twice.
    REQUIRE(clockwork_port_bus_attach(p, 1) == 0);

    lanes_test::tick();
    const float* out = clockwork_audio_out();
    REQUIRE(out != nullptr);
    for (uint32_t c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < bl; ++i)
            REQUIRE(out[c * bl + i] ==
                    static_cast<float>(c + 1) + static_cast<float>(i) / 1000.0f);
    REQUIRE(clockwork_port_readable(p) == 0);
    REQUIRE(clockwork_port_underruns(p) == 0);

    // Detach, and the next block is whatever the host put there again — the
    // port stops owning the range the instant it is unbound.
    clockwork_port_bus_detach(p);
    REQUIRE(clockwork_port_bus_attached() == 0);
    for (uint32_t ch = 0; ch < lanes_test::kInChannels; ++ch)
        for (uint32_t i = 0; i < bl; ++i) in[ch * bl + i] = -5.0f;
    lanes_test::tick();
    out = clockwork_audio_out();
    for (uint32_t i = 0; i < bl; ++i) REQUIRE(out[i] == -5.0f);

    // Put the placeholder back for whatever runs next.
    const auto restore = osc_test::message("/dummy/pulse", 10, 500);
    REQUIRE(lanes_test::ingress(restore.ptr(), restore.size(), 0));
    lanes_test::tick();
    lanes_test::drainRt();
}

TEST_CASE("ports: a sink takes the DSP's output", "[ports][boundary][audio]") {
    Ports guard;
    const uint32_t bl = lanes_test::boot();

    const ClockworkPort p = clockwork_port_open("boundary-sink", kClockworkPortSink, 2, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, 0) == 1);

    lanes_test::tick();
    const float* out = clockwork_audio_out();
    REQUIRE(out != nullptr);
    // Copied before the next tick overwrites the block.
    std::vector<float> expected(out, out + bl * lanes_test::kOutChannels);

    clockwork_port_bus_detach(p);
    REQUIRE(clockwork_port_readable(p) == bl);
    std::vector<float> got(bl * 2, -1.0f);
    REQUIRE(clockwork_port_consume(p, got.data(), bl) == bl);
    for (uint32_t i = 0; i < bl; ++i)
        for (uint32_t c = 0; c < 2; ++c)
            REQUIRE(got[i * 2 + c] == expected[c * bl + i]);
    REQUIRE(clockwork_port_overruns(p) == 0);

    // Detached, it receives nothing further.
    lanes_test::tick();
    REQUIRE(clockwork_port_readable(p) == 0);
}

TEST_CASE("ports: a binding survives its neighbours coming and going",
          "[ports][boundary][audio]") {
    // The reason bindings are a table and not "the open sources in slot
    // order": detaching one must leave every other binding on exactly the
    // channels it had.
    Ports guard;
    const uint32_t bl = lanes_test::boot();
    REQUIRE(lanes_test::kOutChannels >= 2);

    const ClockworkPort first  = clockwork_port_open("sink-0", kClockworkPortSink, 1, 1024);
    const ClockworkPort second = clockwork_port_open("sink-1", kClockworkPortSink, 1, 1024);
    REQUIRE(first != CLOCKWORK_PORT_NONE);
    REQUIRE(second != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(first, 0) == 1);
    REQUIRE(clockwork_port_bus_attach(second, 1) == 1);

    lanes_test::tick();
    const float* out = clockwork_audio_out();
    std::vector<float> block(out, out + bl * lanes_test::kOutChannels);

    // Drop the FIRST one. If channel ranges were derived from order, `second`
    // would silently slide onto channel 0 here.
    clockwork_port_bus_detach(first);
    clockwork_port_close(first);
    REQUIRE(clockwork_port_bus_attached() == 1);

    std::vector<float> got(bl, -1.0f);
    REQUIRE(clockwork_port_consume(second, got.data(), bl) == bl);
    for (uint32_t i = 0; i < bl; ++i) REQUIRE(got[i] == block[1 * bl + i]);

    lanes_test::tick();
    const float* out2 = clockwork_audio_out();
    std::vector<float> block2(out2, out2 + bl * lanes_test::kOutChannels);
    REQUIRE(clockwork_port_readable(second) == bl);
    REQUIRE(clockwork_port_consume(second, got.data(), bl) == bl);
    for (uint32_t i = 0; i < bl; ++i) REQUIRE(got[i] == block2[1 * bl + i]);
}
