// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_lanes.cpp — the lanes C ABI (src/lanes/lanes.h) and the shared ring
 * walker (src/lanes/ring_drain.h).
 *
 * Contract pinned here: the boundary a host sees. Ingress write, both egress
 * drains, the NRT egress producer, input validation, and the published arena
 * layout — exercised directly against a test-local arena (clockwork globals
 * are saved/restored so the rest of the suite is untouched).
 *
 * The walker cases pin the consumer semantics every reader relies on:
 * in-place delivery, consume-after-callback, Retain backpressure, the
 * never-wrap/padding convention, cursor repair, corruption resync and
 * sequence-gap counting.
 *
 * Every payload below is arbitrary bytes. Nothing here loads a definition,
 * creates a voice or asserts on rendered audio: what the DSP does with the
 * bytes is its business, and the boot/tick/audio-out path is pinned in
 * test_audio_path.cpp. These hold with any DSP attached — including the
 * placeholder in dsp/dummy.
 */
#include <catch2/catch_test_macros.hpp>

#include "lanes/lanes.h"            // the host ABI under test (self-guards extern "C")
#include "clockwork_client.h"       // the other way of taking egress, which shares its cursor
#include "lanes/lanes_internal.h"   // clockwork_egress_nrt_write — inject NRT frames for the drain test
#include "shared_memory.h"          // arena layout + EgressRoute
#include "lanes/ring_drain.h"       // clockwork_drain_ring — the walker under test
#include "RingBufferWriter.h"       // inject frames for walker/drain tests
#include "LanesFixture.h"          // a core for the egress calls
#include "core_state.h"            // the core's arena

#include <atomic>
#include <cstring>
#include <string>
#include <vector>


// The audio-thread OUT-ring writer (audio_processor.cpp, global namespace) —
// exercised here to pin it to the same wire convention the walker reads.
extern bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* metrics);

namespace {

// The fixture's core over a zeroed, test-local arena for the length of a
// test, then over its own again, so no other test in the binary is
// affected. The metrics are the scratch arena's too: the RT egress drain
// counts into them.
struct LanesArena {
    std::vector<uint8_t> buf;
    clockwork::Arena&    arena;
    uint8_t* const       savedBase;
    const bool           savedReady;
    uint8_t* const       base;
    RingCursors* const   cursors;
    PerformanceMetrics* const metrics;

    // The fixture's engine is booted before the swap: booted inside it, the
    // restore below would undo the boot.
    LanesArena()
        : buf(clockwork::kDefaultLayout.arena_bytes, 0),
          arena(*clockwork::arena_of(lanes_test::core())),
          savedBase(arena.at()),
          savedReady(arena.ready.load()),
          base(buf.data()),
          cursors(clockwork::cursors_in(base, clockwork::kDefaultLayout)),
          metrics(clockwork::metrics_in(base)) {
        writeArenaHeader(base, clockwork::kDefaultLayout);   // an arena describes itself, even a test's
        arenaInstanceUp(base);    // and has an instance up, for the in ring's door to take its messages
        arena.base.store(base);
        arena.ready.store(true);
        // A fresh arena: fresh reply rings, and with them fresh sequence-gap tracking.
        clockwork_lanes_reset_rt_out_ring(lanes_test::core());
        clockwork_lanes_reset_nrt_out_ring(lanes_test::core());
    }
    ~LanesArena() {
        arena.base.store(savedBase);
        arena.ready.store(savedReady);
    }
};

// ClockworkEgressFn capture sink.
struct Captured {
    int      frames = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> osc;
    // The address of the last frame captured.
    std::string address() const { return std::string(reinterpret_cast<const char*>(osc.data())); }
};
void capture(void* ctx, const uint8_t* osc, uint32_t len, uint32_t seq) {
    auto* c = static_cast<Captured*>(ctx);
    c->frames++; c->seq = seq;
    c->osc.assign(osc, osc + len);
}

// RT egress is written inside clockwork_tick; this puts one frame on the
// OUT ring directly, as the audio thread would. The addresses are arbitrary
// ones that no handler owns — they exist only to prove the lane carries
// bytes, and to tell one frame from another.
constexpr char kFiller[] = "/test/filler";
constexpr char kLater[]  = "/test/latest";
bool writeRtFrame(const char (&address)[13]) {
    std::atomic<int32_t> lock{0};
    uint8_t framed[sizeof(kFiller)];
    std::memcpy(framed, address, sizeof(framed));
    uint8_t* const base = clockwork::laid_out_base(lanes_test::core());
    RingCursors* const cursors = clockwork::cursors_in(base, clockwork::layout_of(lanes_test::core()));
    return RingBufferWriter::write(
        base + clockwork::kDefaultLayout.rt_out_ring_start, clockwork::kDefaultLayout.profile.rt_out_ring_bytes,
        &cursors->out_head, &cursors->out_tail, &cursors->out_sequence, &lock,
        framed, sizeof(framed), 0);
}

// A standalone test ring: buffer + cursors + a writer, independent of the
// arena, for pinning walker semantics on small rings where boundary
// behaviour is easy to force.
struct TestRing {
    std::vector<uint8_t>  buf;
    std::atomic<int32_t>  head{0}, tail{0}, seq{0}, lock{0};
    ClockworkDrainState          st;

    explicit TestRing(uint32_t size) : buf(size, 0) {}
    bool write(const void* data, uint32_t len, uint32_t sourceId = 0) {
        return RingBufferWriter::write(buf.data(),
                                       static_cast<uint32_t>(buf.size()),
                                       &head, &tail, &seq, &lock,
                                       data, len, sourceId);
    }
    template <typename Fn>
    uint32_t drain(Fn&& fn, uint32_t maxFrames = 0, ClockworkDrainStop* stop = nullptr) {
        return clockwork_drain_ring(buf.data(), static_cast<uint32_t>(buf.size()),
                             &head, &tail, st, ClockworkDrainMetrics{}, maxFrames,
                             std::forward<Fn>(fn), stop);
    }
};

}  // namespace

// ── Walker semantics ─────────────────────────────────────────────────────────

TEST_CASE("walker: contiguous payload is delivered in place (no copy)", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {10, 20, 30, 40};   // arbitrary bytes
    REQUIRE(ring.write(msg, sizeof(msg)));

    const uint8_t* seen = nullptr;
    uint32_t n = ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        seen = p;
        REQUIRE(len == sizeof(msg));
        return ClockworkDrainVerdict::Consume;
    });
    REQUIRE(n == 1);
    // The payload pointer must lie inside the ring itself, not a copy.
    REQUIRE(seen >= ring.buf.data());
    REQUIRE(seen < ring.buf.data() + ring.buf.size());
    REQUIRE(std::memcmp(seen, msg, sizeof(msg)) == 0);
}

TEST_CASE("walker: tail advances only after the callback consumes", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {1, 2, 3, 4};
    REQUIRE(ring.write(msg, sizeof(msg)));
    const int32_t tailBefore = ring.tail.load();

    ring.drain([&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        // Mid-callback the frame is still owned by the consumer: writers
        // measure free space against the tail, which must not have moved.
        REQUIRE(ring.tail.load() == tailBefore);
        return ClockworkDrainVerdict::Consume;
    });
    REQUIRE(ring.tail.load() == ring.head.load());  // consumed after return
}

TEST_CASE("walker: Retain leaves the frame in the ring for the next drain", "[lanes][walker]") {
    TestRing ring(1024);
    // Arbitrary 4 bytes; the address shape exists only to prove the transport
    // carries bytes back unchanged, and no handler ever sees it.
    const uint8_t msg[] = {'/', 'r', 0, 0};
    REQUIRE(ring.write(msg, sizeof(msg), 21));

    ClockworkDrainStop stop;
    uint32_t n = ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return ClockworkDrainVerdict::Retain;  // e.g. the consumer's queue is full
    }, 0, &stop);
    REQUIRE(n == 0);                       // retained frames are not counted
    REQUIRE(stop == ClockworkDrainStop::Retained);
    REQUIRE(ring.tail.load() != ring.head.load());  // still queued

    // Next drain delivers the same frame intact.
    std::vector<uint8_t> got;
    uint32_t src = 0;
    n = ring.drain([&](uint32_t s, const uint8_t* p, uint32_t len, uint32_t) {
        src = s; got.assign(p, p + len);
        return ClockworkDrainVerdict::Consume;
    }, 0, &stop);
    REQUIRE(n == 1);
    REQUIRE(src == 21);
    REQUIRE(got == std::vector<uint8_t>(msg, msg + sizeof(msg)));
    REQUIRE(ring.tail.load() == ring.head.load());
}

TEST_CASE("writer: frame that misses the boundary is padded and stays contiguous", "[lanes][walker]") {
    TestRing ring(128);

    // First frame fills 80 bytes; consume it so there is room at the front.
    uint8_t filler[64];
    std::memset(filler, 0xAA, sizeof(filler));
    REQUIRE(ring.write(filler, sizeof(filler)));
    REQUIRE(ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return ClockworkDrainVerdict::Consume;
    }) == 1);

    // Second frame (64 bytes) doesn't fit in the 48 bytes before the end:
    // the writer must emit a padding marker at 80 and restart at offset 0.
    uint8_t msg[48];
    for (uint32_t i = 0; i < sizeof(msg); ++i) msg[i] = static_cast<uint8_t>(i);
    REQUIRE(ring.write(msg, sizeof(msg)));

    uint32_t padMagic;
    std::memcpy(&padMagic, ring.buf.data() + 80, sizeof(padMagic));
    REQUIRE(padMagic == PADDING_MAGIC);
    REQUIRE(ring.head.load() == 64);  // frame restarted at 0, 64 bytes long

    const uint8_t* seen = nullptr;
    std::vector<uint8_t> got;
    REQUIRE(ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        seen = p; got.assign(p, p + len);
        return ClockworkDrainVerdict::Consume;
    }) == 1);
    REQUIRE(seen == ring.buf.data() + sizeof(Message));  // in place at offset 0
    REQUIRE(got == std::vector<uint8_t>(msg, msg + sizeof(msg)));
    REQUIRE(ring.tail.load() == ring.head.load());
}

TEST_CASE("writer: rejects a frame with free space but no contiguous room", "[lanes][walker]") {
    TestRing ring(256);
    uint8_t small[16] = {}, big[128] = {}, probe[80] = {};

    REQUIRE(ring.write(small, sizeof(small)));   // frame 32  @ 0
    REQUIRE(ring.write(big, sizeof(big)));       // frame 144 @ 32 → head 176
    REQUIRE(ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return ClockworkDrainVerdict::Consume;
    }, 1) == 1);                                 // tail 32

    // 96-byte frame: 111 bytes free in total, but only 80 before the end and
    // only 31 before the tail at the front — must be rejected, head untouched.
    REQUIRE_FALSE(ring.write(probe, sizeof(probe)));
    REQUIRE(ring.head.load() == 176);
}

TEST_CASE("walker: frame claiming more bytes than published is corruption, not a stall", "[lanes][walker]") {
    TestRing ring(128);
    // Valid magic with a bit-flipped length of 64 while only 24 bytes are
    // published: waiting for the rest would wedge the lane forever (writers
    // publish head only after the complete frame).
    Message hdr;
    hdr.magic = MESSAGE_MAGIC;
    hdr.length = 64;
    hdr.sequence = 0;
    hdr.sourceId = 0;
    std::memcpy(ring.buf.data(), &hdr, sizeof(hdr));
    ring.tail.store(0);
    ring.head.store(24);

    std::atomic<uint32_t> corrupted{0};
    ClockworkDrainMetrics m; m.corrupted = &corrupted;
    ClockworkDrainStop stop;
    uint32_t n = clockwork_drain_ring(ring.buf.data(), static_cast<uint32_t>(ring.buf.size()),
                               &ring.head, &ring.tail, ring.st, m, 0,
                               [](uint32_t, const uint8_t*, uint32_t, uint32_t) {
                                   return ClockworkDrainVerdict::Consume;
                               }, &stop);
    REQUIRE(n == 0);
    REQUIRE(stop == ClockworkDrainStop::BadLength);
    REQUIRE(corrupted.load() == 1);
    REQUIRE(ring.tail.load() == ring.head.load());  // resynced, not waiting
}

TEST_CASE("OUT writer (ring_buffer_write) follows the unified convention: exact length, aligned advance", "[lanes][walker]") {
    // ring_buffer_write is the audio-thread OUT-ring writer; its frames are
    // drained by the same walker, so a non-4-multiple payload must round-trip
    // exactly and leave the head 4-aligned.
    std::vector<uint8_t> buf(128, 0);
    std::atomic<int32_t> head{0}, tail{0}, seq{0};
    std::atomic<uint32_t> flags{0};

    const uint8_t midi[3] = {0x90, 60, 100};  // not a 4-byte multiple
    REQUIRE(ring_buffer_write(buf.data(), 128, &head, &tail, &seq,
                              5, midi, sizeof(midi), &flags, nullptr));
    // Frame: 16 header + 3 payload = 19 exact, 20-byte footprint.
    REQUIRE(head.load() == 20);

    ClockworkDrainState st;
    uint32_t gotSrc = 0;
    std::vector<uint8_t> gotPayload;
    REQUIRE(clockwork_drain_ring(buf.data(), 128, &head, &tail, st, ClockworkDrainMetrics{}, 0,
        [&](uint32_t src, const uint8_t* p, uint32_t n, uint32_t) {
            gotSrc = src;
            gotPayload.assign(p, p + n);
            return ClockworkDrainVerdict::Consume;
        }) == 1);
    REQUIRE(gotSrc == 5);
    REQUIRE(gotPayload == std::vector<uint8_t>(midi, midi + sizeof(midi)));  // exact, no pad
    REQUIRE(tail.load() == 20);  // aligned advance matches the writer
}

TEST_CASE("walker: frame crossing the ring boundary is corruption, not an OOB read", "[lanes][walker]") {
    TestRing ring(128);

    // Hand-craft a hostile wrap-split frame: valid magic at offset 80 with a
    // length that runs past the end of the ring.
    Message hdr;
    hdr.magic = MESSAGE_MAGIC;
    hdr.length = 64;  // 80 + 64 > 128
    hdr.sequence = 0;
    hdr.sourceId = 0;
    std::memcpy(ring.buf.data() + 80, &hdr, sizeof(hdr));
    ring.tail.store(80);
    ring.head.store(16);  // as if the frame wrapped to 16

    std::atomic<uint32_t> corrupted{0};
    ClockworkDrainMetrics m; m.corrupted = &corrupted;
    ClockworkDrainStop stop;
    int delivered = 0;
    clockwork_drain_ring(ring.buf.data(), static_cast<uint32_t>(ring.buf.size()),
                  &ring.head, &ring.tail, ring.st, m, 0,
                  [&](uint32_t, const uint8_t*, uint32_t, uint32_t) {
                      delivered++;
                      return ClockworkDrainVerdict::Consume;
                  }, &stop);
    REQUIRE(delivered == 0);
    REQUIRE(stop == ClockworkDrainStop::BadLength);
    REQUIRE(corrupted.load() == 1);
    REQUIRE(ring.tail.load() == ring.head.load());  // resynced
}

TEST_CASE("writer: non-aligned payload round-trips exactly; cursors stay 4-aligned", "[lanes][walker]") {
    TestRing ring(128);
    const uint8_t odd[5] = {1, 2, 3, 4, 5};  // MIDI-style: not a 4-byte multiple
    REQUIRE(ring.write(odd, sizeof(odd)));

    std::vector<uint8_t> got;
    REQUIRE(ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        got.assign(p, p + len);
        return ClockworkDrainVerdict::Consume;
    }) == 1);
    // Header length is exact, so the payload size round-trips untouched —
    // while the cursor advances by the 4-aligned footprint (16+5 → 24).
    REQUIRE(got == std::vector<uint8_t>({1, 2, 3, 4, 5}));
    REQUIRE(ring.tail.load() == 24);
}

TEST_CASE("walker: out-of-range tail is repaired to head", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {1, 2, 3, 4};
    REQUIRE(ring.write(msg, sizeof(msg)));
    ring.tail.store(static_cast<int32_t>(ring.buf.size()) + 5);

    ClockworkDrainStop stop;
    uint32_t n = ring.drain([](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return ClockworkDrainVerdict::Consume;
    }, 0, &stop);
    REQUIRE(n == 0);
    REQUIRE(stop == ClockworkDrainStop::BadCursor);
    REQUIRE(ring.tail.load() == ring.head.load());  // repaired, not dereferenced
}

TEST_CASE("walker: padding marker is followed within a single drain call", "[lanes][walker]") {
    TestRing ring(128);

    // Hand-build the never-wrap convention: padding marker near the end, a
    // real frame at offset 0 (this is the OUT-ring writer's wire format).
    // The 4 payload bytes are arbitrary filler — nothing parses them.
    const uint8_t payload[] = {'/', 'f', 0, 0};
    Message hdr;
    hdr.magic = MESSAGE_MAGIC;
    hdr.length = sizeof(Message) + sizeof(payload);
    hdr.sequence = 0;
    hdr.sourceId = 5;
    std::memcpy(ring.buf.data(), &hdr, sizeof(hdr));
    std::memcpy(ring.buf.data() + sizeof(hdr), payload, sizeof(payload));

    Message pad;
    pad.magic = PADDING_MAGIC;
    pad.length = 0;
    pad.sequence = 0;
    pad.sourceId = 0;
    std::memcpy(ring.buf.data() + 96, &pad, sizeof(pad));

    ring.tail.store(96);                                   // parked before the padding
    ring.head.store(static_cast<int32_t>(hdr.length));     // frame at 0 published

    std::vector<uint8_t> got;
    REQUIRE(ring.drain([&](uint32_t, const uint8_t* p, uint32_t len, uint32_t) {
        got.assign(p, p + len);
        return ClockworkDrainVerdict::Consume;
    }) == 1);  // padding followed AND frame delivered in one call
    REQUIRE(got == std::vector<uint8_t>(payload, payload + sizeof(payload)));
}

TEST_CASE("walker: sequence gaps are counted by missed-frame count", "[lanes][walker]") {
    TestRing ring(1024);
    const uint8_t msg[] = {1, 2, 3, 4};
    std::atomic<uint32_t> gaps{0};
    ClockworkDrainMetrics m; m.seqGaps = &gaps;
    auto consume = [](uint32_t, const uint8_t*, uint32_t, uint32_t) {
        return ClockworkDrainVerdict::Consume;
    };

    REQUIRE(ring.write(msg, sizeof(msg)));   // seq 0
    ring.seq.fetch_add(3);                   // frames 1..3 lost
    REQUIRE(ring.write(msg, sizeof(msg)));   // seq 4
    clockwork_drain_ring(ring.buf.data(), static_cast<uint32_t>(ring.buf.size()),
                  &ring.head, &ring.tail, ring.st, m, 0, consume);
    REQUIRE(gaps.load() == 3);
}

// ── Lanes ABI ────────────────────────────────────────────────────────────────

TEST_CASE("lanes ABI: clockwork_ingress_write frames a message onto the IN ring", "[lanes][abi]") {
    LanesArena arena;
    const uint8_t msg[] = {1, 2, 3, 4, 5, 6, 7, 8};   // arbitrary bytes
    REQUIRE(clockwork_ingress_write(lanes_test::core(), msg, sizeof(msg), 7));

    // Drain the IN ring with the shared walker; the frame must round-trip.
    ClockworkDrainState st;
    int frames = 0; uint32_t gotSrc = 0; std::vector<uint8_t> gotPayload;
    uint32_t delivered = clockwork_drain_ring(
        arena.base + clockwork::kDefaultLayout.in_ring_start, clockwork::kDefaultLayout.profile.in_ring_bytes,
        &arena.cursors->in_head, &arena.cursors->in_tail, st, ClockworkDrainMetrics{}, 0,
        [&](uint32_t src, const uint8_t* p, uint32_t n, uint32_t) {
            frames++; gotSrc = src; gotPayload.assign(p, p + n);
            return ClockworkDrainVerdict::Consume;
        });
    REQUIRE(delivered == 1);
    REQUIRE(frames == 1);
    REQUIRE(gotSrc == 7);
    REQUIRE(gotPayload == std::vector<uint8_t>(msg, msg + sizeof(msg)));
}

TEST_CASE("lanes ABI: NRT egress write -> drain round-trips the payload", "[lanes][abi]") {
    LanesArena arena;
    const uint8_t payload[] = {'/', 'x', 0, 0};   // arbitrary filler bytes
    REQUIRE(clockwork_egress_nrt_write(lanes_test::core(), payload, sizeof(payload)));

    Captured cap;
    REQUIRE(clockwork_egress_nrt_drain(lanes_test::core(), capture, &cap, 0) == 1);
    REQUIRE(cap.frames == 1);
    REQUIRE(cap.osc == std::vector<uint8_t>(payload, payload + sizeof(payload)));
}

TEST_CASE("lanes ABI: RT egress drain delivers a framed OUT frame", "[lanes][abi]") {
    LanesArena arena;
    REQUIRE(writeRtFrame(kFiller));

    Captured cap;
    REQUIRE(clockwork_egress_rt_drain(lanes_test::core(), capture, &cap, 0) == 1);
    REQUIRE(cap.address() == kFiller);
}

// The egress rings have one read cursor, and both ways of taking from them
// move it: these drains, and clockwork_client_poll. A host may hand the
// stream from one to the other — a fixture that drains and then attaches a
// client, a host that opens a client after draining at boot — and the one
// that takes over must start where the other stopped, neither handing out
// again what was taken nor reading bytes already given back to the writer.
TEST_CASE("lanes ABI: a client polling after the egress drains takes nothing they took", "[lanes][abi]") {
    LanesArena arena;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkClient* c = clockwork_client_open_memory(arena.base, clockwork::kDefaultLayout.arena_bytes, &st);
    REQUIRE(c != nullptr);

    const uint8_t payload[] = {'/', 'x', 0, 0};   // arbitrary filler bytes
    REQUIRE(writeRtFrame(kFiller));
    REQUIRE(clockwork_egress_nrt_write(lanes_test::core(), payload, sizeof(payload)));
    Captured rt, nrt;
    REQUIRE(clockwork_egress_rt_drain(lanes_test::core(), capture, &rt, 0) == 1);
    REQUIRE(clockwork_egress_nrt_drain(lanes_test::core(), capture, &nrt, 0) == 1);

    ClockworkClientMessage m[8];
    CHECK(clockwork_client_poll(c, m, 8) == 0);

    // And what arrives after the handover is the client's, once.
    REQUIRE(writeRtFrame(kLater));
    const uint32_t n = clockwork_client_poll(c, m, 8);
    REQUIRE(n == 1);
    CHECK(std::string(reinterpret_cast<const char*>(m[0].bytes)) == kLater);
    CHECK(clockwork_client_poll(c, m, 8) == 0);
    clockwork_client_close(c);
}

TEST_CASE("lanes ABI: the egress drains after a client poll take nothing it took", "[lanes][abi]") {
    LanesArena arena;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkClient* c = clockwork_client_open_memory(arena.base, clockwork::kDefaultLayout.arena_bytes, &st);
    REQUIRE(c != nullptr);

    const uint8_t payload[] = {'/', 'x', 0, 0};   // arbitrary filler bytes
    REQUIRE(writeRtFrame(kFiller));
    REQUIRE(clockwork_egress_nrt_write(lanes_test::core(), payload, sizeof(payload)));
    ClockworkClientMessage m[8];
    REQUIRE(clockwork_client_poll(c, m, 8) == 2);
    clockwork_client_close(c);

    Captured again;
    CHECK(clockwork_egress_rt_drain(lanes_test::core(), capture, &again, 0) == 0);
    CHECK(clockwork_egress_nrt_drain(lanes_test::core(), capture, &again, 0) == 0);
    CHECK(again.frames == 0);

    // And what arrives after the handover is the drain's, once.
    REQUIRE(writeRtFrame(kLater));
    Captured next;
    REQUIRE(clockwork_egress_rt_drain(lanes_test::core(), capture, &next, 0) == 1);
    CHECK(next.address() == kLater);
    CHECK(clockwork_egress_rt_drain(lanes_test::core(), capture, &next, 0) == 0);
}

TEST_CASE("lanes ABI: ingress/egress reject bad input and empty drains", "[lanes][abi]") {
    LanesArena arena;
    REQUIRE_FALSE(clockwork_ingress_write(lanes_test::core(), nullptr, 4, 0));
    REQUIRE_FALSE(clockwork_ingress_write(lanes_test::core(), reinterpret_cast<const uint8_t*>("x"), 0, 0));

    Captured cap;
    REQUIRE(clockwork_egress_rt_drain(lanes_test::core(), capture, &cap, 0) == 0);   // nothing queued
    REQUIRE(clockwork_egress_nrt_drain(lanes_test::core(), capture, &cap, 0) == 0);
    REQUIRE(cap.frames == 0);
}

TEST_CASE("lanes ABI: the arena's own table describes the compiled arena", "[lanes][abi]") {
    // A host that addresses the rings directly reads the table at the front
    // of the arena instead of the headers, so it must agree with them entry
    // for entry (clockwork_arena.h).
    LanesArena arena;
    const ClockworkArenaHeader* h = clockwork_arena_header(lanes_test::core());
    REQUIRE(h != nullptr);
    REQUIRE(h == arenaHeader(clockwork_lanes_base(lanes_test::core())));
    REQUIRE(h->magic == CLOCKWORK_ARENA_MAGIC);
    REQUIRE(h->version == CLOCKWORK_ARENA_VERSION);
    REQUIRE(h->state == CLOCKWORK_ARENA_PUBLISHED);
    REQUIRE(h->arena_bytes == clockwork::kDefaultLayout.arena_bytes);

    const auto region = [&](uint32_t id) {
        const ClockworkArenaEntry* e = clockwork_arena_find(h, id);
        REQUIRE(e != nullptr);
        return e;
    };
    const auto* in  = region(CLOCKWORK_ARENA_CLOCKWORK_IN_RING);
    const auto* out = region(CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING);
    const auto* nrt = region(CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING);
    REQUIRE(in->offset  == clockwork::kDefaultLayout.in_ring_start);      REQUIRE(in->bytes  == clockwork::kDefaultLayout.profile.in_ring_bytes);
    REQUIRE(out->offset == clockwork::kDefaultLayout.rt_out_ring_start);     REQUIRE(out->bytes == clockwork::kDefaultLayout.profile.rt_out_ring_bytes);
    REQUIRE(nrt->offset == clockwork::kDefaultLayout.nrt_out_ring_start); REQUIRE(nrt->bytes == clockwork::kDefaultLayout.profile.nrt_out_ring_bytes);
    REQUIRE(region(CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS)->offset == clockwork::kDefaultLayout.ring_cursors_start);
    REQUIRE(region(CLOCKWORK_ARENA_CLOCKWORK_METRICS)->offset == METRICS_START);
    REQUIRE(region(CLOCKWORK_ARENA_CLOCKWORK_METRICS)->geom[CLOCKWORK_GEOM_METRICS_FIELDS] == METRICS_SIZE / 4u);

    // The wire constants a foreign reader needs to parse a frame at all.
    REQUIRE(in->geom[CLOCKWORK_GEOM_RING_MESSAGE_MAGIC] == MESSAGE_MAGIC);
    REQUIRE(in->geom[CLOCKWORK_GEOM_RING_PADDING_MAGIC] == PADDING_MAGIC);
    REQUIRE(in->geom[CLOCKWORK_GEOM_RING_HEADER_BYTES] == sizeof(Message));

    // Every ring must lie wholly inside the arena the table advertises.
    REQUIRE(in->offset + in->bytes <= h->arena_bytes);
    REQUIRE(out->offset + out->bytes <= h->arena_bytes);
    REQUIRE(nrt->offset + nrt->bytes <= h->arena_bytes);

    // A host sizes its block buffers from this before it ever ticks, so it
    // must be usable at any point in the process's life. The exact value is a
    // boot-time decision and is pinned in test_audio_path.cpp.
    REQUIRE(clockwork_block_size(lanes_test::core()) > 0u);
}

TEST_CASE("lanes ABI: a frame either egress ring refuses, for want of room or for its size, is counted as refused, and as nothing else",
          "[lanes][abi][egress]") {
    // A refused reply is one a client never gets, and the line logging it
    // rides the same full ring, so the count is the only trace.
    // messages_dropped is the way in's; the way out has its own.
    LanesArena arena;
    const uint8_t payload[60] = {'/', 'x', 0, 0};

    // The NRT ring: written until it refuses, then three more.
    int written = 0;
    while (clockwork_egress_nrt_write(lanes_test::core(), payload, sizeof(payload))) ++written;
    REQUIRE(written > 0);
    for (int i = 0; i < 3; ++i) REQUIRE_FALSE(clockwork_egress_nrt_write(lanes_test::core(), payload, sizeof(payload)));
    CHECK(arena.metrics->egress_frames_refused.load() == 4u);

    // The RT ring, through the writer the audio thread uses.
    std::atomic<uint32_t> status{0};
    while (ring_buffer_write(arena.base + clockwork::kDefaultLayout.rt_out_ring_start, clockwork::kDefaultLayout.profile.rt_out_ring_bytes,
                             &arena.cursors->out_head, &arena.cursors->out_tail, &arena.cursors->out_sequence,
                             0, payload, sizeof(payload), &status, arena.metrics)) {}
    CHECK(arena.metrics->egress_frames_refused.load() == 5u);
    CHECK((status.load() & STATUS_BUFFER_FULL) != 0u);

    // A frame too big for the NRT ring ever to carry is refused and counted
    // too, as the RT ring counts one: a guest's answer sent from a thread of
    // its own goes no further, and the client gets nothing. (Clockwork's own
    // producer also answers one with an error, OscEgress.)
    std::vector<uint8_t> huge(clockwork_egress_nrt_max_frame(lanes_test::core()) + 1, 0);
    CHECK_FALSE(clockwork_egress_nrt_write(lanes_test::core(), huge.data(), static_cast<uint32_t>(huge.size())));
    CHECK(arena.metrics->egress_frames_refused.load() == 6u);

    // Nothing on the way in was refused.
    CHECK(arena.metrics->messages_dropped.load() == 0u);
}
