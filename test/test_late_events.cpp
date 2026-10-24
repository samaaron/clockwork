// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_late_events.cpp — a timed event that comes due late. By default it
 * plays, late, and is counted late: clockwork never drops for lateness. A
 * host may set the latest a timed event may play
 * (clockwork_set_timed_event_late_limit_ms, 0 = any); a later one is then
 * dropped, counted as such — apart from the want-of-room drops — and said.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "clockwork_client.h"
#include "lanes/lanes.h"
#include "metrics_schema.h"

#include <string>
#include <vector>

namespace {

// The engine's metrics block, as a client reads it (clockwork_client_metrics).
struct Counts { uint32_t lates, dropped, lateDropped; };
Counts counts(EngineFixture& fix) {
    std::vector<uint32_t> words(clockwork::metrics_schema::kMetricWords, 0);
    const uint32_t n = clockwork_client_metrics(fix.engine().egressClient(), words.data(), static_cast<uint32_t>(words.size()));
    REQUIRE(n > 64);
    return { words[8], words[5], words[64] };   // scheduler_lates, scheduler_queue_dropped, scheduler_late_dropped
}

ClockworkEngine::Config pumped() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}

// The engine's clock as a client reads it.
double clockNtp(EngineFixture& fix) {
    ClockworkClientSampleClock sc{};
    sc.struct_bytes = sizeof(sc);
    REQUIRE(clockwork_client_sample_clock(fix.engine().egressClient(), &sc) == CLOCKWORK_OK);
    return sc.dac_ntp;
}
uint64_t timetag(double ntp) { return static_cast<uint64_t>(ntp * 4294967296.0); }

int linesSaying(EngineFixture& fix, const char* text) {
    for (int i = 0; i < 20; ++i) fix.engine().controlPass();
    fix.pollUntil([] { return false; }, 100);
    int n = 0;
    for (const auto& line : fix.debugMessages())
        if (line.find(text) != std::string::npos) ++n;
    return n;
}

// The limit is the process's: back to the default whatever a case did.
struct LimitGuard { ~LimitGuard() { clockwork_set_timed_event_late_limit_ms(0); } };

}  // namespace

TEST_CASE("late events: a timed event that comes due late plays, late, and is counted late; nothing is dropped "
          "for lateness by default", "[scheduler][late][engine]") {
    LimitGuard guard;
    clockwork_set_timed_event_late_limit_ms(0);
    EngineFixture fix(pumped());
    const Counts before = counts(fix);

    // Two seconds late already as it is sent.
    fix.send(osc_test::bundle(timetag(clockNtp(fix) - 2.0), { osc_test::message("/dummy/events/get") }));
    OscReply r;
    REQUIRE(fix.waitForReply("/dummy/events", r));
    const Counts after = counts(fix);
    CHECK(after.lates == before.lates + 1);
    CHECK(after.dropped == before.dropped);
    CHECK(after.lateDropped == before.lateDropped);
}

TEST_CASE("late events: with the latest a timed event may play set by the host, a later one is dropped, counted "
          "as such, and said", "[scheduler][late][engine]") {
    LimitGuard guard;
    EngineFixture fix(pumped());
    clockwork_set_timed_event_late_limit_ms(500);
    const Counts before = counts(fix);
    OscReply r;

    // Within the limit: plays, late.
    fix.send(osc_test::bundle(timetag(clockNtp(fix) - 0.3), { osc_test::message("/dummy/events/get") }));
    REQUIRE(fix.waitForReply("/dummy/events", r));

    // Beyond it: dropped, counted as late and dropped (not as want of room), said once.
    fix.send(osc_test::bundle(timetag(clockNtp(fix) - 2.0), { osc_test::message("/dummy/events/get") }));
    CHECK_FALSE(fix.waitForReply("/dummy/events", r, 300));
    const Counts after = counts(fix);
    CHECK(after.lateDropped == before.lateDropped + 1);
    CHECK(after.dropped == before.dropped);
    CHECK(linesSaying(fix, "was dropped: the host allows 500 ms") == 1);

    // Back to any lateness: plays again.
    clockwork_set_timed_event_late_limit_ms(0);
    fix.send(osc_test::bundle(timetag(clockNtp(fix) - 2.0), { osc_test::message("/dummy/events/get") }));
    REQUIRE(fix.waitForReply("/dummy/events", r));
}

TEST_CASE("metrics history: a client reads what a counter did over the last second, through the API",
          "[metrics][history][engine]") {
    EngineFixture fix(pumped());
    fix.pumpBlock(fix.blocksOfAudio(1500));   // a second and a half of blocks, on the engine's own clock
    ClockworkClientMetricWindow w{};
    w.struct_bytes = sizeof w;
    // Blocks rendered (process_count, word 0): the last ten seconds hold the full second rendered.
    REQUIRE(clockwork_client_metrics_history(fix.engine().egressClient(), 0, 10, 0, &w) == CLOCKWORK_OK);
    CHECK(w.covered >= 2);
    CHECK(w.delta > 300);
    CHECK(w.last >= w.high);
    // A constant has no history.
    REQUIRE(clockwork_client_metrics_history(fix.engine().egressClient(), 0, 1, 42, &w) == CLOCKWORK_OK);
    CHECK(w.covered == 0);
    // Out of range asks are refused.
    CHECK(clockwork_client_metrics_history(fix.engine().egressClient(), 3, 1, 0, &w) == CLOCKWORK_OK);   // a level past the last: an empty window
    CHECK(w.covered == 0);
}

