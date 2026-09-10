// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_client_abi.cpp — the client boundary, and the claim that it is the only
 * door.
 *
 * src/clockwork_client.h says a remote peer's traffic and an embedder's client
 * meet the same code. That is worth asserting rather than believing: the whole
 * value of the boundary is that there is one implementation of the ring
 * arithmetic, and a second path reintroduced quietly would look exactly like
 * this file still passing.
 *
 * So the cases below drive an engine through the ABI and through ingest() —
 * which is what a UDP datagram reaches — and require both to arrive.
 */
#include "EngineFixture.h"
#include "clockwork_client.h"
#include "lanes/lanes.h"
#include "shared_memory.h"
#include "shm_scope_stream.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

// The engine's arena, as audio_processor.cpp publishes it: the same base an
// in-process client addresses.
extern "C" { extern uint8_t* shared_memory; }

#include <cstring>
#include <string>
#include <vector>

using engine_test::Engine;

namespace {

// A handle onto the engine this process is running, which is what a plugin's
// GUI has and what a WebAssembly build has: same address space, no segment.
ClockworkClient* openInProcess(ClockworkStatus* st) {
    return clockwork_client_open_memory(shared_memory, TOTAL_BUFFER_SIZE, st);
}

} // namespace

TEST_CASE("client abi: a message sent through the boundary reaches the guest",
          "[client][abi]") {
    Engine e;

    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);
    REQUIRE(c != nullptr);

    // Ordinary traffic, from a client that is not the engine.
    const auto ping = osc_test::message("/dummy/ping");
    CHECK(clockwork_client_send(c, ping.ptr(), ping.size(), 0xC11E) == CLOCKWORK_OK);

    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    clockwork_client_close(c);
}

TEST_CASE("client abi: the engine's own ingest goes through the same door",
          "[client][abi]") {
    // THE POINT OF THE BOUNDARY. ClockworkEngine::ingest is what a UDP
    // datagram reaches after the transport has minted an origin for its
    // sender, and it calls clockwork_client_send rather than writing the ring
    // itself. A client's message and a remote peer's therefore travel the same
    // code, and this asserts they land alike.
    Engine e;

    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto viaAbi = osc_test::message("/dummy/ping");
    CHECK(clockwork_client_send(c, viaAbi.ptr(), viaAbi.size(), 0xAAAA) == CLOCKWORK_OK);
    osc_test::ParsedReply r1;
    REQUIRE(e.reply("/dummy/pong", r1));

    // The transport's route, with a different origin so the two are told apart.
    const auto viaIngest = osc_test::message("/dummy/ping");
    e.engine.ingest(viaIngest.ptr(), viaIngest.size(), 0xBBBB);
    osc_test::ParsedReply r2;
    REQUIRE(e.reply("/dummy/pong", r2));

    clockwork_client_close(c);
}

TEST_CASE("client abi: a message built in the ring arrives like any other",
          "[client][abi][reserve]") {
    // A caller whose bytes do not exist yet reserves room and builds the frame
    // where it will live. The engine cannot tell the difference, which is the
    // whole point: this is the same door, entered without a buffer.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto ping = osc_test::message("/dummy/ping");

    uint8_t* room = clockwork_client_send_begin(c, ping.size(), &st);
    REQUIRE(st == CLOCKWORK_OK);
    REQUIRE(room != nullptr);
    std::memcpy(room, ping.ptr(), ping.size());
    REQUIRE(clockwork_client_send_commit(c, ping.size(), 0xD00D) == CLOCKWORK_OK);

    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    clockwork_client_close(c);
}

TEST_CASE("client abi: the ring advances by what was used, not what was asked for",
          "[client][abi][reserve]") {
    // THE PROPERTY THAT MAKES A GENEROUS BOUND FREE. An encoder knows an upper
    // bound before it knows a length, so it reserves the bound; if the ring
    // charged for the bound, a client that guessed high would fill it in a
    // handful of messages.
    //
    // Reserving 64 KiB and committing a few dozen bytes, far more times than
    // the ring could hold at 64 KiB apiece, is the assertion.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto ping = osc_test::message("/dummy/ping");
    const uint32_t bound = 64 * 1024;
    const uint32_t rounds = (IN_BUFFER_SIZE / bound) * 4;
    REQUIRE(rounds > 4);

    // NOTHING IS DRAINED WHILE THIS RUNS, which is what makes it an
    // assertion. Pumping between sends would free space either way and the
    // count would come out the same whether the ring charged for the bound or
    // for the message — an earlier version of this case did exactly that and
    // passed against a writer that charged the bound.
    uint32_t sent = 0;
    while (true) {
        uint8_t* room = clockwork_client_send_begin(c, bound, &st);
        if (room == nullptr) break;
        std::memcpy(room, ping.ptr(), ping.size());
        REQUIRE(clockwork_client_send_commit(c, ping.size(), 1) == CLOCKWORK_OK);
        ++sent;
        if (sent > 100000) break;   // a runaway is a failure, not a hang
    }

    // Charging the bound would fit a dozen; charging the message fits
    // thousands. The gap is wide enough that no tuning sits between them.
    CHECK(sent > rounds * 20);

    e.pump(0.5);
    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    clockwork_client_close(c);
}

TEST_CASE("client abi: a reservation given back leaves the ring working",
          "[client][abi][reserve]") {
    // Abandoning a reservation is the one way to stop this ring for good, so
    // the two ways of ending one are worth pinning: abort publishes nothing
    // and releases, and a bad length does the same rather than holding on.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto ping = osc_test::message("/dummy/ping");

    // Abort.
    REQUIRE(clockwork_client_send_begin(c, 128, &st) != nullptr);
    clockwork_client_send_abort(c);

    // Committing more than was reserved: refused, and NOT still holding the
    // lock — the send below would hang for ever if it were.
    uint8_t* room = clockwork_client_send_begin(c, 16, &st);
    REQUIRE(room != nullptr);
    CHECK(clockwork_client_send_commit(c, 4096, 0) == CLOCKWORK_E_ARG);

    // Commit with nothing open is an argument fault, not a crash.
    CHECK(clockwork_client_send_commit(c, 4, 0) == CLOCKWORK_E_ARG);
    // And abort with nothing open is simply accepted.
    clockwork_client_send_abort(c);

    // The ring still works, which is what all of the above was protecting.
    CHECK(clockwork_client_send(c, ping.ptr(), ping.size(), 7) == CLOCKWORK_OK);
    osc_test::ParsedReply r;
    REQUIRE(e.reply("/dummy/pong", r));

    clockwork_client_close(c);
}

TEST_CASE("client abi: a second reservation is refused rather than deadlocked",
          "[client][abi][reserve]") {
    // Beginning twice would take a lock this thread already holds. That is a
    // hang, not an error, so it is caught before the lock is touched — and a
    // test that hangs instead of failing is the reason to pin it.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    REQUIRE(clockwork_client_send_begin(c, 64, &st) != nullptr);

    ClockworkStatus second = CLOCKWORK_OK;
    CHECK(clockwork_client_send_begin(c, 64, &second) == nullptr);
    CHECK(second == CLOCKWORK_E_ARG);

    clockwork_client_send_abort(c);

    // Bad arguments, and a bound the ring can never satisfy, both refuse
    // without taking the lock — proven by the working send afterwards.
    CHECK(clockwork_client_send_begin(c, 0, &second) == nullptr);
    CHECK(second == CLOCKWORK_E_ARG);
    CHECK(clockwork_client_send_begin(c, IN_BUFFER_SIZE + 1024, &second) == nullptr);
    CHECK(second == CLOCKWORK_E_TOO_BIG);

    const auto ping = osc_test::message("/dummy/ping");
    CHECK(clockwork_client_send(c, ping.ptr(), ping.size(), 3) == CLOCKWORK_OK);

    clockwork_client_close(c);
}

TEST_CASE("client abi: refusals are told apart", "[client][abi]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    const auto ping = osc_test::message("/dummy/ping");

    // Bad arguments are not backpressure and must not read as it: a caller
    // that retries on E_FULL would spin forever on a null pointer.
    CHECK(clockwork_client_send(c, nullptr, 4, 0) == CLOCKWORK_E_ARG);
    CHECK(clockwork_client_send(c, ping.ptr(), 0, 0) == CLOCKWORK_E_ARG);

    // Too big is a verdict, not a moment. A frame larger than the ring will
    // never fit however long a caller waits, so it is refused differently from
    // a ring that happens to be full.
    std::vector<uint8_t> huge(IN_BUFFER_SIZE + 1024, 0);
    CHECK(clockwork_client_send(c, huge.data(),
                                static_cast<uint32_t>(huge.size()), 0) == CLOCKWORK_E_TOO_BIG);

    clockwork_client_close(c);
}

TEST_CASE("client abi: regions are named, and absent is an answer",
          "[client][abi]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkRegion reg{};
    REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_METRICS, &reg) == CLOCKWORK_OK);
    CHECK(reg.base == shared_memory + METRICS_START);
    CHECK(reg.bytes == METRICS_SIZE);
    CHECK(reg.writable == 0);          // the engine's to write, not ours

    REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_WINDOW, &reg) == CLOCKWORK_OK);
    CHECK(reg.bytes == SHM_WINDOW_SIZE);

    // The device layer's account of its callback, beside the metrics: the
    // same flat-uint32 shape, the region the schema's nativeStats table
    // indexes, and nothing a client may write.
    REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_NATIVE_STATS, &reg) == CLOCKWORK_OK);
    CHECK(reg.base == shared_memory + NATIVE_STATS_START);
    CHECK(reg.bytes == NATIVE_STATS_SIZE);
    CHECK(reg.writable == 0);

    // A region this engine does not expose here says so rather than handing
    // back a pointer into whatever is next along.
    CHECK(clockwork_client_region(c, CLOCKWORK_REGION_INBOX, &reg) == CLOCKWORK_E_ABSENT);

    clockwork_client_close(c);
}

TEST_CASE("client abi: an older caller's struct is not written past",
          "[client][abi]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    // A caller built against a smaller version of the struct sets its own
    // size. The library must fill that much and no more — this is the whole
    // mechanism that lets the two sides be different ages, so it is asserted
    // by watching a guard byte past the end.
    struct { ClockworkClientInfo info; uint32_t guard; } probe{};
    probe.guard = 0xFEEDFACEu;
    probe.info.struct_bytes = sizeof(uint32_t) * 4;   // pretend to be an old build

    REQUIRE(clockwork_client_info(c, &probe.info) == CLOCKWORK_OK);
    CHECK(probe.guard == 0xFEEDFACEu);
    CHECK(probe.info.abi_version == CLOCKWORK_CLIENT_ABI_VERSION);
    // Beyond the declared size nothing was written, so the tail is still zero.
    CHECK(probe.info.sample_rate == 0.0);

    // A caller of this build's size gets the geometry: what the engine was
    // initialised with, which is what a client sizes its buffers by.
    ClockworkClientInfo full{};
    full.struct_bytes = sizeof(full);
    REQUIRE(clockwork_client_info(c, &full) == CLOCKWORK_OK);
    CHECK(full.sample_rate == Catch::Approx(clockwork_sample_rate()));
    CHECK(full.block_frames == clockwork_block_size());
    CHECK(full.output_channels > 0);
    CHECK(full.features & CLOCKWORK_FEATURE_SCOPE);

    clockwork_client_close(c);
}

TEST_CASE("client abi: a scope slot reads as silence unless it is live", "[client][abi]") {
    // A slot that was never claimed, or was released, still holds whatever
    // its ring last carried. The header promises a reader silence for it —
    // `out` always fully written — so a display never draws a dead slot's
    // last window as if it were sounding now.
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkScopeReader r{};
    r.struct_bytes = sizeof(r);
    REQUIRE(clockwork_client_scope_open(c, 0, &r) == CLOCKWORK_OK);
    CHECK(clockwork_client_scope_valid(&r) == 0);

    const uint32_t frames = 64;
    std::vector<float> out(frames * SHM_SCOPE_STREAM_CHANNELS, 7.0f);
    uint32_t channels = 0;
    CHECK(clockwork_client_scope_read(c, &r, UINT64_MAX, frames, out.data(), &channels) == 0);
    CHECK(channels == SHM_SCOPE_STREAM_CHANNELS);
    for (float s : out) REQUIRE(s == 0.0f);

    // The slot's producer claims it and writes a ramp: the reader sees the
    // ramp, right-aligned to the write edge.
    auto* slot = reinterpret_cast<shm_scope_stream*>(shared_memory + SHM_SCOPE_START + SHM_SCOPE_HEADER_SIZE);
    shm_scope_stream_writer w(slot);
    w.activate(2);
    std::vector<float> l(256), rr(256);
    for (uint32_t i = 0; i < 256; ++i) { l[i] = static_cast<float>(i); rr[i] = -static_cast<float>(i); }
    const float* block[2] = { l.data(), rr.data() };
    w.write(block, 256, 0);
    CHECK(clockwork_client_scope_valid(&r) == 1);
    std::fill(out.begin(), out.end(), 7.0f);
    REQUIRE(clockwork_client_scope_read(c, &r, UINT64_MAX, frames, out.data(), &channels) == frames);
    CHECK(channels == 2);
    for (uint32_t f = 0; f < frames; ++f) {
        INFO("frame " << f);
        REQUIRE(out[f * 2] == static_cast<float>(192 + f));
        REQUIRE(out[f * 2 + 1] == -static_cast<float>(192 + f));
    }

    // Released: the ring still holds the ramp, and the reader reports none of it.
    slot->state.store(0, std::memory_order_release);
    CHECK(clockwork_client_scope_valid(&r) == 0);
    std::fill(out.begin(), out.end(), 7.0f);
    CHECK(clockwork_client_scope_read(c, &r, UINT64_MAX, frames, out.data(), &channels) == 0);
    for (float s : out) REQUIRE(s == 0.0f);

    clockwork_client_close(c);
}

TEST_CASE("client abi: the clock arrives as one snapshot", "[client][abi]") {
    Engine e;
    ClockworkStatus st = CLOCKWORK_E_ARG;
    ClockworkClient* c = openInProcess(&st);
    REQUIRE(st == CLOCKWORK_OK);

    ClockworkClientClock clk{};
    clk.struct_bytes = sizeof(clk);
    REQUIRE(clockwork_client_clock(c, &clk) == CLOCKWORK_OK);

    CHECK(clk.bpm > 0.0);
    CHECK(clk.meter_num > 0);
    CHECK(clk.meter_den > 0);

    // The beat maths is the library's, so every binding agrees to the bit
    // rather than each re-deriving it: one beat later is one beat later.
    const double t0 = clk.beat_origin_ntp;
    const double b0 = clockwork_client_beat_at(&clk, t0);
    const double b1 = clockwork_client_beat_at(&clk, t0 + 60.0 / clk.bpm);
    CHECK(b1 - b0 == Catch::Approx(1.0).epsilon(1e-9));

    clockwork_client_close(c);
}

TEST_CASE("client abi: opening rejects memory that cannot hold an engine",
          "[client][abi]") {
    // A caller that passes a buffer too small is a caller that has the wrong
    // pointer, and finding that out at open is cheaper than reading rubbish
    // out of the middle of something else.
    uint8_t tiny[64] = {};
    ClockworkStatus st = CLOCKWORK_OK;
    CHECK(clockwork_client_open_memory(tiny, sizeof tiny, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ARG);

    CHECK(clockwork_client_open_memory(nullptr, TOTAL_BUFFER_SIZE, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ARG);

    // And every status has words, because a binding raising an exception has
    // to put something in it.
    CHECK(std::string(clockwork_client_status_text(CLOCKWORK_E_TOO_BIG)).size() > 0);
}

// ── The two ways this engine says where its rings are ────────────────────────
//
// A client takes the layout FROM THE SEGMENT (ShmReaderLayout::from_arena, via
// shm_segment_client): "the ENGINE's layout, not this build's", so a client
// built separately still finds the rings. The audio thread does not — its drain
// addresses them with compile-time constants:
//
//     clockwork_drain_ring(shared_memory + IN_BUFFER_START, IN_BUFFER_SIZE,
//                          &control->in_head, &control->in_tail, ...)
//
// Two sources of truth for one address. They agree only while the table the
// engine publishes matches the constants the engine compiled, and nothing
// asserted that they do. If they ever diverge the failure is silent in the
// worst way: a client's send lands where the drain never looks, so the ring the
// engine reads stays empty forever. Nothing is dropped and nothing is
// corrupted — there is simply never anything there — and the only visible
// symptom is that messages are never answered.
//
// Asserted here rather than inferred from a timeout somewhere downstream.
TEST_CASE("the arena's published layout is the one the audio thread compiled",
          "[client][layout]") {
    Engine e;   // boots the engine, which writes the arena header

    ShmReaderLayout table{};
    const char* why = nullptr;
    REQUIRE(ShmReaderLayout::from_arena(shared_memory, TOTAL_BUFFER_SIZE, table, &why));
    INFO("from_arena refused: " << (why ? why : "-"));

    const ShmReaderLayout k = ShmReaderLayout::from_constants();

    // The ingress path first: this is the one whose disagreement would strand a
    // client's traffic where the drain cannot see it.
    CHECK(table.control_offset == k.control_offset);
    CHECK(table.control_bytes  == k.control_bytes);
    CHECK(table.in_ring_offset == k.in_ring_offset);
    CHECK(table.in_ring_size   == k.in_ring_size);

    // And the rest of the boundary, for the same reason one step removed.
    CHECK(table.out_ring_offset     == k.out_ring_offset);
    CHECK(table.out_ring_size       == k.out_ring_size);
    CHECK(table.nrt_out_ring_offset == k.nrt_out_ring_offset);
    CHECK(table.nrt_out_ring_size   == k.nrt_out_ring_size);
    CHECK(table.metrics_offset      == k.metrics_offset);
    CHECK(table.metrics_bytes       == k.metrics_bytes);
    CHECK(table.blob_size           == k.blob_size);
}
