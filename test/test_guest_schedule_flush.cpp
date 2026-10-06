// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_guest_schedule_flush.cpp — a guest's "clear the schedule" clears it.
 *
 * A client sends its notes early, as timestamped bundles, and clockwork holds
 * them until each falls due (SCHED_TAG_SYNTH). The guest holds none of them,
 * so its own clearing verb (scsynth's /clearSched) cleared nothing: Sonic Pi
 * pressed Stop, and half a second of notes still played; the engine rebuilt
 * for a device change, and they played into the empty world. DspHost::
 * flush_schedule is the guest's way to drop them; the dummy guest's
 * /dummy/flush calls it.
 *
 * The case is the clock (manual pump, freewheel): nothing falls due unless
 * the case renders it, so a bundle dated by the wall clock half a second
 * ahead is reached by rendering a second of blocks, however slow the machine.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "clock/clock_math.h"   // wallClockNTP
#include <cmath>
#include <cstdint>

namespace {

ClockworkEngine::Config theCaseKeepsTheClock() {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    cfg.freewheelClock  = true;
    return cfg;
}

uint64_t ntpTimetag(double seconds) {
    const double whole = std::floor(seconds);
    return (static_cast<uint64_t>(whole) << 32)
         | static_cast<uint64_t>((seconds - whole) * 4294967296.0);
}

// A ping sent half a second ahead, as a client sends its notes.
void pingAhead(EngineFixture& fix) {
    fix.send(osc_test::bundle(ntpTimetag(wallClockNTP() + 0.5),
                              { osc_test::message("/dummy/ping") }));
}

uint32_t blocksForSeconds(double s) {
    return static_cast<uint32_t>(s * engine_test::kSampleRate / get_audio_buffer_samples());
}

}  // namespace

TEST_CASE("GuestScheduleFlush: a bundle sent ahead reaches the guest when it falls due",
          "[scheduler][guest]") {
    EngineFixture fix(theCaseKeepsTheClock());
    pingAhead(fix);
    fix.pumpBlock(blocksForSeconds(2.0));
    OscReply pong;
    CHECK(fix.waitForReply("/dummy/pong", pong, 200));
}

TEST_CASE("GuestScheduleFlush: the guest clearing its schedule drops what was sent ahead",
          "[scheduler][guest]") {
    EngineFixture fix(theCaseKeepsTheClock());
    pingAhead(fix);
    fix.send(osc_test::message("/dummy/flush"));
    fix.pumpBlock(blocksForSeconds(2.0));
    OscReply pong;
    INFO(fix.debugMessagesDump());
    CHECK_FALSE(fix.waitForReply("/dummy/pong", pong, 200));
}
