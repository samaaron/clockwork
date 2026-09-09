// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_dsp_regions.cpp — the REGION half of src/dsp_api.h.
 *
 * The boundary moves data in three shapes and this file is about the third.
 * Messages are pinned in test_dsp_boundary.cpp and test_osc_ingress.cpp; audio is
 * pinned in test_audio_path.cpp and test_ports.cpp. Regions had nothing at
 * all: DspConfig hands the guest five blocks of shared memory with five
 * different sets of promises attached, and until this file not one of those
 * promises was checked by anything.
 *
 *   guest_config   host writes, guest reads, clockwork only carries
 *   guest_memory   clockwork reserves and zeroes, guest owns outright
 *   shm_window     guest writes, clients poll
 *   persistent     guest writes, survives the guest's own destruction
 *   clock          clockwork writes, guest and clients read
 *
 * WHY THE ASSERTIONS GO THROUGH THE GUEST
 *
 * A region is by definition not observable from outside the thing that owns
 * it. Reading `shared_memory + GUEST_CONFIG_START` from a test would prove
 * that clockwork put bytes in the arena — which is not the claim. The claim
 * is that the GUEST is handed those bytes, at the right address, with the
 * right length, in the state promised. So the placeholder DSP reports what it
 * was given (its /dummy/region/...  verbs) and the assertions are made on its
 * answers.
 *
 * The exception is shm_window, which is asserted from the arena directly, and
 * for the mirror-image reason: that region's entire purpose is that a client
 * outside the guest can read it. Asking the guest what it published would
 * prove only that it remembers.
 *
 * WHY THE SENTINELS ARE SILLY NUMBERS
 *
 * 0xC0FFEE01..04 cannot be arrived at by accident. A test asserting that a
 * config slot is 4 would pass against a harness that lost the block entirely
 * and left the region zeroed on a machine where 4 happened to be there. These
 * assert transport, not plausibility.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"
#include "shared_memory.h"
#include "audio_processor.h"
#include "dsp_api.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Declared here rather than via audio_processor.h, which pulls in the
// <emscripten/...> shim that is not on a native include path. Both have C
// linkage (src/audio_processor.cpp). test_stream_identity.cpp does the same.
extern "C" {
extern struct Dsp* g_dsp;
void destroy_dsp();
void rebuild_dsp(double sample_rate);
}

namespace {

constexpr uint32_t kClient = 0x9e61;

const lanes_test::EgressFrame* findAddress(
    const std::vector<lanes_test::EgressFrame>& frames, const char* address) {
    for (const auto& f : frames) {
        if (osc_test::parseAddress(f.data.data(),
                                   static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    }
    return nullptr;
}

// Ask the guest one question and get its answer, or fail the test. Every case
// below is "ask, then assert", so the asking is factored out — but the drain
// is NOT hidden inside it: a stale frame from a previous case answering this
// one is exactly the mistake this helper could otherwise cause, so callers
// drain first and see themselves doing it.
osc_test::ParsedReply ask(const char* verb, const char* replyAddress) {
    const auto q = osc_test::message(verb);
    REQUIRE(lanes_test::ingress(q.ptr(), q.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, replyAddress);
    const auto* reply = findAddress(frames, replyAddress);
    REQUIRE(reply != nullptr);
    // Answered to the client that asked, never broadcast. A region report that
    // reached the wrong client would pass every value assertion below.
    REQUIRE(reply->sourceId == kClient);
    return osc_test::parseReply(reply->data.data(),
                                static_cast<uint32_t>(reply->data.size()));
}

} // namespace

// ── guest_config: host writes, guest reads, clockwork carries ─────────────────

TEST_CASE("regions: the guest is handed the host's config block, byte for byte",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto r = ask("/dummy/region/config", "/dummy/region/config-is");

    // The length is the REGION's, not the block's: clockwork hands over the
    // whole region because it does not know where the host's bytes stop. A
    // guest that needs to know reads its own length out of its own layout.
    REQUIRE(r.argInt(0) == static_cast<int32_t>(GUEST_CONFIG_SIZE));

    // The sentinels the fixture wrote, in order, unchanged. This is the claim
    // that survived the 2026-09-01 change: clockwork copies these bytes and
    // reads none of them.
    REQUIRE(static_cast<uint32_t>(r.argInt(1)) == lanes_test::kGuestConfigSlots[0]);
    REQUIRE(static_cast<uint32_t>(r.argInt(2)) == lanes_test::kGuestConfigSlots[1]);
    REQUIRE(static_cast<uint32_t>(r.argInt(3)) == lanes_test::kGuestConfigSlots[2]);
    REQUIRE(static_cast<uint32_t>(r.argInt(4)) == lanes_test::kGuestConfigSlots[3]);
}

TEST_CASE("regions: clockwork's geometry reaches the guest without a round trip",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto r = ask("/dummy/region/geometry", "/dummy/region/geometry-is");

    // What the fixture told clockwork_init, arriving as DspConfig fields. Until
    // 2026-09-01 three of these four made a round trip through the config
    // block above — the host wrote them into it and init_memory read them
    // back — so a host that wanted to say "two channels" had to know one
    // engine's option layout. The assertion that matters is that these agree
    // with the ARGUMENTS, while the slots asserted above are the host's
    // private sentinels: the two are now completely independent, and a
    // regression that reconnected them would have to change one to match the
    // other.
    REQUIRE(r.argInt(0) == static_cast<int32_t>(lanes_test::kSampleRate));
    REQUIRE(r.argInt(1) == static_cast<int32_t>(lanes_test::kBufLength));
    REQUIRE(r.argInt(2) == static_cast<int32_t>(lanes_test::kInChannels));
    REQUIRE(r.argInt(3) == static_cast<int32_t>(lanes_test::kOutChannels));

    // And the block size the guest was told is the one the host actually
    // renders at, which is a separate claim: clockwork_block_size is what a host
    // sizes its own buffers from.
    REQUIRE(clockwork_block_size() == lanes_test::kBufLength);
}

// ── guest_memory: reserved, zeroed, and the guest's alone ───────────────────

TEST_CASE("regions: the guest's memory arrives zeroed, whole, and writable",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto r = ask("/dummy/region/memory", "/dummy/region/memory-is");

    REQUIRE(r.argInt(0) == static_cast<int32_t>(lanes_test::kGuestMemoryBytes));
    // Zeroed at dsp_new — checked over EVERY byte by the guest, not sampled.
    REQUIRE(r.argInt(1) == 1);
    // And really that long: the guest wrote and read back the first byte, the
    // middle and the last, so a region shorter than its stated length shows up
    // here rather than as a heap corruption somewhere else entirely.
    REQUIRE(r.argInt(2) == 1);
}

TEST_CASE("regions: clockwork does not write inside the guest's memory",
          "[boundary][regions]") {
    lanes_test::boot();

    // A PATTERN, not zeros. "Still all zero" only catches a harness that wrote
    // something non-zero; a pattern catches one that wrote anything at all,
    // including a stray memset — which is the more likely mistake, since
    // zeroing this region IS something clockwork legitimately does at
    // dsp_new and must not do at any other moment.
    //
    // Writing it from the test rather than through the guest also keeps the
    // case independent of what else has run: the guest uses this region as
    // scratch for outbound blobs, so an all-zero assertion would depend on
    // test order when the whole binary runs in one process.
    auto* mem = const_cast<uint8_t*>(lanes_test::guestMemory());
    REQUIRE(mem != nullptr);
    for (uint32_t i = 0; i < lanes_test::kGuestMemoryBytes; ++i)
        mem[i] = static_cast<uint8_t>(i * 31u + 7u);

    // Run blocks: the claim is not "it was clean at boot" but "nothing here
    // ever touches it", and a harness scribbling once per block would pass a
    // boot-time check.
    for (int i = 0; i < 8; ++i) lanes_test::tick();

    size_t changed = 0;
    for (uint32_t i = 0; i < lanes_test::kGuestMemoryBytes; ++i)
        if (mem[i] != static_cast<uint8_t>(i * 31u + 7u)) ++changed;
    REQUIRE(changed == 0);
}

// ── The tiers, and the wants that are checked against them ──────────────────

TEST_CASE("regions: the guest's memory wants are declared, checked, and met",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // The placeholder declares wants (dsp_describe), and this fixture is a
    // single-region host: no bulk tier, so the fast arena has to hold both.
    // That the boot got this far IS the check passing; what is asserted here
    // is that the arithmetic the boot used agrees with the declaration.
    const DspInfo* info = dsp_describe();
    REQUIRE(info != nullptr);
    REQUIRE(info->arena_bytes_wanted > 0);
    REQUIRE(info->arena_bulk_bytes_wanted > 0);
    REQUIRE(info->arena_bytes_wanted + info->arena_bulk_bytes_wanted
            <= lanes_test::kGuestMemoryBytes);
    REQUIRE(g_dsp != nullptr);

    // What the guest was handed. A single-region host: no bulk arena, and the
    // guest reports "nothing to check" for it rather than a failure.
    const auto r = ask("/dummy/region/tiers", "/dummy/region/tiers-is");
    REQUIRE(r.argInt(0) == 0);   // arena_bulk_bytes
    REQUIRE(r.argInt(1) == 1);   // bulk_was_zero: vacuously
    REQUIRE(r.argInt(2) == 1);   // bulk_rw_ok:    vacuously
    // The floating-point environment the fixture declared reached the guest
    // verbatim — DENORMALS_HONOURED, because the fixture arms nothing on the
    // thread that ticks. A host that says nothing would show UNKNOWN here.
    REQUIRE(r.argInt(3) == DSP_FP_ENV_DENORMALS_HONOURED);
}

TEST_CASE("regions: a want the host cannot meet is refused, never spilled",
          "[boundary][regions]") {
    // The rule itself (audio_processor.h), pinned in isolation: the boot path
    // runs it once with the placeholder's wants, which are met, so the
    // refusals can only be exercised here.
    uint32_t sf = 0, sb = 0;

    // Zero wants are met by any host, including one offering nothing.
    CHECK(clockwork_arena_meets(0, 0, /*tiered*/ 0, 0, 0, &sf, &sb) != 0);
    CHECK(clockwork_arena_meets(0, 0, /*tiered*/ 1, 0, 0, &sf, &sb) != 0);

    // Single-region: the fast arena carries both wants.
    CHECK(clockwork_arena_meets(1000, 0, 0, 600, 400, &sf, &sb) != 0);
    CHECK(clockwork_arena_meets(999,  0, 0, 600, 400, &sf, &sb) == 0);
    CHECK(sf == 1);
    CHECK(sb == 0);
    // ...and a bulk arena that happens to be offered on a single-region host
    // does not rescue a fast shortfall, because the rule says there is no
    // bulk tier there.
    CHECK(clockwork_arena_meets(999, 4096, 0, 600, 400, &sf, &sb) == 0);

    // Tiered: each want against its own tier, and NO SPILL in either
    // direction — a fast shortfall is not met from a roomy bulk, nor a bulk
    // shortfall from a roomy fast.
    CHECK(clockwork_arena_meets(600, 400, 1, 600, 400, &sf, &sb) != 0);
    CHECK(clockwork_arena_meets(599, 4096, 1, 600, 400, &sf, &sb) == 0);
    CHECK(sf == 1);
    CHECK(sb == 0);
    CHECK(clockwork_arena_meets(4096, 399, 1, 600, 400, &sf, &sb) == 0);
    CHECK(sf == 0);
    CHECK(sb == 1);

    // Two wants near the top of uint32 do not wrap into "met".
    CHECK(clockwork_arena_meets(UINT32_MAX, 0, 0, UINT32_MAX, 16, &sf, &sb) == 0);
    CHECK(sf == 16);

    // The shortfall outputs are optional.
    CHECK(clockwork_arena_meets(1, 0, 0, 2, 0, nullptr, nullptr) == 0);
}

// ── shm_window: the guest publishes, a client polls ─────────────────────────

TEST_CASE("regions: what the guest publishes through the window is readable outside it",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // The record the placeholder publishes. Transcribed rather than shared,
    // deliberately: a client reading this region is a separate program with
    // its own idea of the layout, and a test that included the guest's header
    // would prove only that a struct equals itself.
    struct Window { uint32_t magic, seq, value, writes; };
    constexpr uint32_t kWindowMagic = 0x57494e44u;   // 'WIND'

    const auto* base = static_cast<const uint8_t*>(clockwork_lanes_base());
    REQUIRE(base != nullptr);
    const auto* w = reinterpret_cast<const Window*>(base + SHM_WINDOW_START);

    const uint32_t writesBefore = (w->magic == kWindowMagic) ? w->writes : 0u;

    const auto pub = osc_test::message("/dummy/region/window", 0x1234abcd, 0);
    REQUIRE(lanes_test::ingress(pub.ptr(), pub.size(), kClient));
    lanes_test::tickUntil(4, nullptr);

    REQUIRE(w->magic == kWindowMagic);
    REQUIRE(w->value == 0x1234abcdu);
    REQUIRE(w->writes == writesBefore + 1);
    // Settled, not mid-write. The publisher makes the sequence odd while it
    // writes and even when it is done, so an even number here is the claim
    // that a poller can tell the difference — the reason the region is safe to
    // read from another thread at all.
    REQUIRE((w->seq % 2) == 0);

    // A second publication moves both, so the reader can tell a fresh value
    // from a stale one even when the value itself repeats.
    const auto pub2 = osc_test::message("/dummy/region/window", 0x1234abcd, 0);
    REQUIRE(lanes_test::ingress(pub2.ptr(), pub2.size(), kClient));
    lanes_test::tickUntil(4, nullptr);

    REQUIRE(w->value == 0x1234abcdu);
    REQUIRE(w->writes == writesBefore + 2);
    REQUIRE((w->seq % 2) == 0);
}

// ── clock: clockwork writes, the guest reads ──────────────────────────────

TEST_CASE("regions: the guest reads the session clock clockwork publishes",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto r = ask("/dummy/region/clock", "/dummy/region/clock-is");

    // The pointer was supplied at all — DspConfig::clock may legitimately be
    // NULL in a build with no clock, and this build has one.
    REQUIRE(r.argInt(0) == 1);
    // A real tempo, read through readClockworkClock rather than field by field.
    // Reported in millibeats so it survives the trip as an int32.
    REQUIRE(r.argInt(1) > 0);
}

TEST_CASE("regions: a tempo written into the region reaches the guest",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto before = ask("/dummy/region/clock", "/dummy/region/clock-is");
    const int32_t bpmBefore = before.argInt(1);
    REQUIRE(bpmBefore == 120000);   // the default initDefaults leaves

    // Write the region the way its OWNER does — as the bit pattern of a
    // double, released — and ask the guest what it now reads.
    //
    // Not through /clockwork/clock/tempo/set, deliberately. That verb writes
    // through a bound ClockworkClock, and a host driving lanes.h alone binds none
    // (lanes.h: "leave g_active_clockwork_clock null and the engine's clock/MIDI
    // paths stay inert"). Going through it here would test the clock
    // subsystem's presence, not the region — and would silently pass forever
    // by asserting nothing, because an inert verb changes nothing.
    //
    // What IS under test is the region contract: one writer, N readers, no
    // message carries the value, and the guest sees what the writer left.
    auto* state = reinterpret_cast<ClockworkClockState*>(
        static_cast<uint8_t*>(clockwork_lanes_base()) + CLOCK_STATE_START);
    const uint64_t saved = state->bpm.load(std::memory_order_acquire);

    state->bpm.store(clockwork::doubleToBits(132.0), std::memory_order_release);

    const auto after = ask("/dummy/region/clock", "/dummy/region/clock-is");
    REQUIRE(after.argInt(1) == 132000);
    REQUIRE(after.argInt(1) != bpmBefore);

    // Put it back: the fixture is a process singleton, and a tempo left at 132
    // is a surprise for whatever runs next.
    state->bpm.store(saved, std::memory_order_release);
    const auto restored = ask("/dummy/region/clock", "/dummy/region/clock-is");
    REQUIRE(restored.argInt(1) == bpmBefore);
}

// ── persistent: outlives the guest that wrote it ────────────────────────────

TEST_CASE("regions: the config block sits clear of every ring",
          "[boundary][regions]") {
    // A LAYOUT GUARD, checked in constants rather than at runtime. The config
    // region shares one arena with the three OSC rings, so an offset that
    // overlaps one would be written straight through by ordinary traffic —
    // and would look like a guest misreading its own block rather than like
    // the layout fault it is.
    const auto disjoint = [](uint32_t aStart, uint32_t aSize,
                             uint32_t bStart, uint32_t bSize) {
        return aStart + aSize <= bStart || bStart + bSize <= aStart;
    };

    CHECK(disjoint(GUEST_CONFIG_START, GUEST_CONFIG_SIZE, IN_BUFFER_START,      IN_BUFFER_SIZE));
    CHECK(disjoint(GUEST_CONFIG_START, GUEST_CONFIG_SIZE, OUT_BUFFER_START,     OUT_BUFFER_SIZE));
    CHECK(disjoint(GUEST_CONFIG_START, GUEST_CONFIG_SIZE, NRT_OUT_BUFFER_START, NRT_OUT_BUFFER_SIZE));
    CHECK(disjoint(GUEST_CONFIG_START, GUEST_CONFIG_SIZE, CONTROL_START,        CONTROL_SIZE));
    CHECK(disjoint(GUEST_CONFIG_START, GUEST_CONFIG_SIZE, METRICS_START,        METRICS_SIZE));

    // And it is inside the arena it is addressed from.
    CHECK(GUEST_CONFIG_START + GUEST_CONFIG_SIZE <= TOTAL_BUFFER_SIZE);
}

TEST_CASE("regions: the config block survives traffic and a rebuild",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto slots = [] {
        const auto r = ask("/dummy/region/config", "/dummy/region/config-is");
        return std::array<uint32_t, 4>{ static_cast<uint32_t>(r.argInt(1)),
                                        static_cast<uint32_t>(r.argInt(2)),
                                        static_cast<uint32_t>(r.argInt(3)),
                                        static_cast<uint32_t>(r.argInt(4)) };
    };

    const auto before = slots();

    // Ordinary ingress, drained as it goes so the egress ring never backs up
    // and starts dropping — a flood that outruns the drain tests the drain,
    // not the region. If the config block were inside a ring's span, this is
    // the traffic that would scribble on it.
    for (int i = 0; i < 2000; ++i) {
        const auto ping = osc_test::message("/dummy/ping");
        REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), 1u));
        // Tick, not just drain: ingress is consumed by the block, so without
        // one the messages queue up and the next question waits behind them.
        lanes_test::tick();
        lanes_test::drainRt();
    }
    CHECK(slots() == before);

    // A device switch rebuilds the guest. The host's block is copied in again,
    // so the new instance is handed the same bytes rather than whatever the
    // previous one left behind.
    destroy_dsp();
    rebuild_dsp(lanes_test::kSampleRate);
    lanes_test::drainRt();
    CHECK(slots() == before);
}

TEST_CASE("regions: persistent survives a rebuild and the guest's memory does not",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // ONE case, not two, because the claim is the CONTRAST. Both regions are
    // handed to the guest by the same struct at the same moment and look
    // identical from inside it; the only thing that distinguishes them is what
    // happens at a rebuild, and testing them apart would let both drift to the
    // same behaviour with two green tests to show for it.

    // Write something only this instance could have written.
    const auto put = osc_test::message("/dummy/region/persist", 0x5150ab1e, 0);
    REQUIRE(lanes_test::ingress(put.ptr(), put.size(), kClient));
    {
        const auto frames = lanes_test::tickUntil(4, "/dummy/region/persist-gen");
        const auto* gen = findAddress(frames, "/dummy/region/persist-gen");
        REQUIRE(gen != nullptr);
        const auto r = osc_test::parseReply(gen->data.data(),
                                            static_cast<uint32_t>(gen->data.size()));
        REQUIRE(r.argInt(0) >= 1);           // a generation was stamped
    }

    // Also dirty the guest's own memory, so "was it cleared" is a question
    // with a real answer. Reaching in from the test rather than asking the
    // guest to: the region is the guest's, but the fixture reserved it, and
    // this is the fixture checking what clockwork does to what it reserved.
    auto* mem = const_cast<uint8_t*>(lanes_test::guestMemory());
    mem[0] = 0xEE;
    mem[lanes_test::kGuestMemoryBytes - 1] = 0xEE;

    // The device switch. destroy_dsp + rebuild_dsp is exactly what ClockworkEngine
    // does for a cold swap, and it is the moment both promises are tested.
    destroy_dsp();
    rebuild_dsp(lanes_test::kSampleRate);
    REQUIRE(g_dsp != nullptr);

    // PERSISTENT SURVIVED. The new instance found the old one's record, and it
    // validated before believing it — a torn or stale record would report
    // found=0 rather than a plausible wrong number.
    {
        const auto r = ask("/dummy/region/persist-at-birth", "/dummy/region/persist-was");
        REQUIRE(r.argInt(0) == static_cast<int32_t>(GUEST_PERSIST_SIZE));
        REQUIRE(r.argInt(1) == 1);                             // found
        REQUIRE(r.argInt(2) >= 1);                             // its generation
        REQUIRE(static_cast<uint32_t>(r.argInt(3)) == 0x5150ab1eu);   // its value
    }

    // AND THE GUEST'S MEMORY DID NOT. A rebuilt guest's allocator starts from
    // nothing, so the region it allocates in must start from nothing too;
    // handing back the last instance's bytes would let a stale pointer look
    // valid.
    {
        const auto r = ask("/dummy/region/memory", "/dummy/region/memory-is");
        REQUIRE(r.argInt(1) == 1);           // zeroed again at this dsp_new
    }
    REQUIRE(mem[0] == 0);
    REQUIRE(mem[lanes_test::kGuestMemoryBytes - 1] == 0);

    // And the geometry came back identical, which is the rebuild's own claim:
    // rebuild_dsp changes the sample rate and nothing else, so a device switch
    // that did not change the channel count must not change it.
    {
        const auto r = ask("/dummy/region/geometry", "/dummy/region/geometry-is");
        REQUIRE(r.argInt(0) == static_cast<int32_t>(lanes_test::kSampleRate));
        REQUIRE(r.argInt(1) == static_cast<int32_t>(lanes_test::kBufLength));
        REQUIRE(r.argInt(2) == static_cast<int32_t>(lanes_test::kInChannels));
        REQUIRE(r.argInt(3) == static_cast<int32_t>(lanes_test::kOutChannels));
    }

    // The config block came through the rebuild too — the host wrote it once,
    // at boot, and a rebuilt guest still needs to know how it was configured.
    {
        const auto r = ask("/dummy/region/config", "/dummy/region/config-is");
        REQUIRE(static_cast<uint32_t>(r.argInt(1)) == lanes_test::kGuestConfigSlots[0]);
        REQUIRE(static_cast<uint32_t>(r.argInt(4)) == lanes_test::kGuestConfigSlots[3]);
    }
}

TEST_CASE("regions: a blob clockwork has no verb for survives a rebuild",
          "[boundary][regions]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // THIS IS WHAT REPLACED THE DEFINITION CACHE, so it is worth a case of its
    // own rather than being folded into the one above.
    //
    // Clockwork used to keep a copy of every definition a client sent, by
    // being told which verb carried one and asking the guest to read a name
    // out of each blob. It worked for exactly the category somebody had built
    // it for: samples, and anything else a guest invented, had no equivalent
    // and were left to the client to re-send.
    //
    // The replacement makes no distinction, because it never looks. The guest
    // writes what it wants to keep, clockwork guarantees the bytes across a
    // rebuild, and clockwork cannot tell a definition from a sample from
    // anything else — which is the property that makes it work for all three.
    //
    // The value below stands in for a blob whose meaning clockwork has no
    // way to learn. Nothing in the boundary names it, and nothing has to.
    const auto put = osc_test::message("/dummy/region/persist", 0xDEFB10B5, 0);
    REQUIRE(lanes_test::ingress(put.ptr(), put.size(), kClient));
    lanes_test::tickUntil(4, "/dummy/region/persist-gen");

    destroy_dsp();
    rebuild_dsp(lanes_test::kSampleRate);
    REQUIRE(g_dsp != nullptr);

    const auto r = ask("/dummy/region/persist-at-birth", "/dummy/region/persist-was");
    REQUIRE(r.argInt(1) == 1);
    REQUIRE(static_cast<uint32_t>(r.argInt(3)) == 0xDEFB10B5u);
    const int32_t genAtBirth = r.argInt(2);
    REQUIRE(genAtBirth >= 1);

    // And the new instance CONTINUES the sequence rather than restarting it,
    // so a guest can tell "the record I wrote" from "a record some earlier run
    // left behind" — the staleness dsp_api.h warns about, which a cache keyed
    // by name could not express at all.
    //
    // Asserted by writing again and reading the generation back, not by
    // expecting a particular number at birth: ctest runs each case in its own
    // process, so a case that assumed an earlier one had already bumped the
    // counter passed under a filter and failed in the suite.
    const auto again = osc_test::message("/dummy/region/persist", 0x2, 0);
    REQUIRE(lanes_test::ingress(again.ptr(), again.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, "/dummy/region/persist-gen");
    const auto* gen = findAddress(frames, "/dummy/region/persist-gen");
    REQUIRE(gen != nullptr);
    const auto g = osc_test::parseReply(gen->data.data(),
                                        static_cast<uint32_t>(gen->data.size()));
    REQUIRE(g.argInt(0) == genAtBirth + 1);
}
