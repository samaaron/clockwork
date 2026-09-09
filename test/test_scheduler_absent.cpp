// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_scheduler_absent.cpp — what clockwork does with time when it has no
 * timed store. Compiled ONLY into a CLOCKWORK_SCHEDULER=OFF build (see
 * test/CMakeLists.txt); its twin, test_scheduler.cpp, is compiled only into an
 * ON one. Neither is skipped at runtime, because a case that reports "passed"
 * without running proves nothing.
 *
 * The option removes the store outright: no slot pool, no data pool, and the
 * Rust `store` half not compiled. What that costs is /clockwork/schedule,
 * /clockwork/sched/flush, timed MIDI out and timed OSC forwarding. What it must
 * NOT cost is delivery, and that is what this file pins:
 *
 *   - the placeholder DSP declares holds_schedule = 0, i.e. it ASKS
 *     clockwork to hold timed messages for it — and this build cannot. That is
 *     the configuration most likely to surprise someone, so it is the one
 *     tested: the message must still arrive, at once, carrying its ORIGINAL
 *     timetag rather than being dropped or flattened to "now". The DSP asked
 *     for something the build cannot provide; handing it the message with the
 *     time intact and letting it decide is the honest answer, and the timetag
 *     is the only thing it could decide anything with.
 *
 *   - /clockwork/schedule refuses, and refuses LOUDLY. A verb that silently
 *     stops working is the worst of the three outcomes, so the refusal is
 *     counted into the scheduler drop metric where a client can see it.
 *
 *   - immediate messages are untouched. The option is about time, not about
 *     the message path.
 *
 * The timetag is read back through /dummy/when, which answers /dummy/when-was
 * with the int64 dsp_osc() was called with. Nothing else about a message's
 * time is observable from outside the boundary.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"
#include "dsp_api.h"
#include "clock/clock_math.h"
#include "clockwork_prefix.h"
#include "shared_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if CLOCKWORK_SCHEDULER
#error "test_scheduler_absent.cpp belongs to a CLOCKWORK_SCHEDULER=OFF build only"
#endif

// Clockwork's arena globals (audio_processor.cpp). Declared directly rather
// than via audio_processor.h, which pulls in the <emscripten/...> shim that is
// not on this target's include path — the same reason test_lanes.cpp does it.
extern "C" {
    extern PerformanceMetrics* metrics;
}

namespace {

constexpr uint32_t kClient = 0x5eed;   // not 0: 0 is the broadcast route

const lanes_test::EgressFrame* findAddress(
    const std::vector<lanes_test::EgressFrame>& frames, const char* address) {
    for (const auto& f : frames) {
        if (osc_test::parseAddress(f.data.data(),
                                   static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    }
    return nullptr;
}

// The int64 argument of a "/dummy/when-was ,h <t>" reply.
int64_t whenWasArg(const lanes_test::EgressFrame& f) {
    const auto r = osc_test::parseReply(f.data.data(),
                                        static_cast<uint32_t>(f.data.size()));
    REQUIRE(r.argCount() == 1);
    return r.argInt64(0);
}

// Ask the DSP what timetag it was handed for `pkt`, ticking until the answer
// arrives. `maxBlocks` is generous — the point of every case here is that the
// answer comes back immediately, and each asserts HOW immediately itself.
int64_t askWhen(const osc_test::Packet& pkt, int maxBlocks = 8) {
    lanes_test::drainRt();
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));
    const auto frames = lanes_test::tickUntil(maxBlocks, "/dummy/when-was");
    const auto* f = findAddress(frames, "/dummy/when-was");
    REQUIRE(f != nullptr);
    CHECK(f->sourceId == kClient);
    return whenWasArg(*f);
}

}  // namespace

// ── The declaration this build cannot honour ─────────────────────────────────

TEST_CASE("no scheduler: the DSP asks clockwork to hold, and it cannot",
          "[scheduler][absent][boundary]") {
    // Stated rather than assumed: the whole point of the cases below is that
    // this DSP WANTS clockwork to hold timed messages. A placeholder that
    // declared holds_schedule = 1 would make them pass for the wrong reason.
    const DspInfo* info = dsp_describe();
    REQUIRE(info != nullptr);
    CHECK(info->holds_schedule == 0);
}

// ── Delivery survives; the time survives with it ─────────────────────────────

TEST_CASE("no scheduler: an immediate message still reaches the DSP",
          "[scheduler][absent][boundary]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, "/dummy/pong");
    const auto* pong = findAddress(frames, "/dummy/pong");
    REQUIRE(pong != nullptr);
    CHECK(pong->sourceId == kClient);
}

TEST_CASE("no scheduler: a timed bundle arrives at once, with its timetag intact",
          "[scheduler][absent][boundary][timing]") {
    // The case the option is most likely to surprise someone with, and the one
    // the composition rule in src/scheduler/engine_schedule.h turns on: this
    // DSP declared holds_schedule = 0, so it ASKED clockwork to hold — and
    // this build has nothing to hold with.
    //
    // A bundle due forty blocks out would, in a build WITH a store, wait until
    // it came due (test_dsp_boundary.cpp asserts exactly that). Here it must be
    // handed over on the very next block, carrying the timetag it was sent
    // with — not "now", not zero. That number is the only thing the DSP could
    // use to work out that it is early, so flattening it would destroy the one
    // piece of information the honest answer depends on.
    const uint32_t bl = lanes_test::boot();
    const uint64_t now = lanes_test::tick();

    constexpr int kBlocksAhead = 40;
    const uint64_t dueFrame = now + static_cast<uint64_t>(kBlocksAhead) * bl;
    const int64_t  due = clockwork::ntpToOscTimetag(lanes_test::ntpAtFrame(dueFrame));
    REQUIRE(due < 0);   // a present-day timetag really is negative as an int64

    const auto inner = osc_test::message("/dummy/when");
    const auto pkt   = osc_test::bundle(static_cast<uint64_t>(due), { inner });

    lanes_test::drainRt();
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));

    // ONE block. Not "eventually": a build with no store has nothing to defer
    // with, so an answer later than the block that drains the ring would mean
    // something is holding it after all.
    const auto frames = lanes_test::tickUntil(1, "/dummy/when-was");
    const auto* f = findAddress(frames, "/dummy/when-was");
    REQUIRE(f != nullptr);
    CHECK(f->sourceId == kClient);
    CHECK(whenWasArg(*f) == due);   // intact, to the sub-sample fraction
    CHECK(whenWasArg(*f) != 1);     // not flattened to "immediately"
    CHECK(whenWasArg(*f) != 0);
}

TEST_CASE("no scheduler: an immediate bundle still means immediate",
          "[scheduler][absent][boundary][timing]") {
    // OSC timetag 1 is "now" and must stay distinguishable from a real time:
    // forwarding everything with a fabricated timetag would pass the case
    // above and break this one.
    lanes_test::boot();
    const auto inner = osc_test::message("/dummy/when");
    const auto pkt   = osc_test::bundle(1u, { inner });
    CHECK(askWhen(pkt, 1) == 1);
}

TEST_CASE("no scheduler: a plain message carries the immediate timetag",
          "[scheduler][absent][boundary][timing]") {
    lanes_test::boot();
    CHECK(askWhen(osc_test::message("/dummy/when"), 1) == 1);
}

// ── The refusal ──────────────────────────────────────────────────────────────

TEST_CASE("no scheduler: /clockwork/schedule refuses and says so, rather than "
          "vanishing", "[scheduler][absent][refusal]") {
    lanes_test::boot();
    lanes_test::drainRt();
    lanes_test::drainNrt();

    REQUIRE(metrics != nullptr);
    const uint32_t before =
        metrics->scheduler_queue_dropped.load(std::memory_order_relaxed);

    // A well-formed "/clockwork/schedule <timetag> <blob>" — well-formed on
    // purpose, so what is being tested is the missing store and not a parse
    // failure that would have been refused in either configuration.
    const auto inner = osc_test::message("/dummy/ping");
    osc_test::Builder b;
    auto& s = b.begin(CLOCKWORK_SYS("schedule"));
    s << static_cast<osc::int64>(clockwork::ntpToOscTimetag(
             lanes_test::ntpAtFrame(lanes_test::tick() + 4800)))
      << osc::Blob(inner.ptr(),
                   static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    const auto pkt = b.end();

    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));
    const auto frames = lanes_test::tickUntil(8, nullptr);

    // Counted where a client can see it: refused is not the same as lost, and
    // the metric is the difference.
    CHECK(metrics->scheduler_queue_dropped.load(std::memory_order_relaxed) == before + 1u);

    // The inner message must NOT have been delivered. Refusing the verb and
    // then quietly performing it immediately would be the worst answer of all
    // — the client asked for a time and would get neither the time nor an
    // error.
    CHECK(findAddress(frames, "/dummy/pong") == nullptr);

    // And the queue depth stays 0, because there is no queue.
    CHECK(metrics->scheduler_queue_depth.load(std::memory_order_relaxed) == 0u);
}

TEST_CASE("no scheduler: a refused schedule does not disturb the next message",
          "[scheduler][absent][refusal]") {
    // The refusal returns early from the classifier. If it left anything in a
    // bad state, the very next message is where it would show.
    lanes_test::boot();

    const auto inner = osc_test::message("/dummy/ping");
    osc_test::Builder b;
    auto& s = b.begin(CLOCKWORK_SYS("schedule"));
    s << static_cast<osc::int64>(1)
      << osc::Blob(inner.ptr(),
                   static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    const auto pkt = b.end();
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));
    lanes_test::tickUntil(2, nullptr);

    lanes_test::drainRt();
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, "/dummy/pong");
    CHECK(findAddress(frames, "/dummy/pong") != nullptr);
}
