// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_time_source.cpp — pins clockwork contract for the native TimeSource
 * (wall clock + drift IIR) and for DriftLogGate, its diagnostic rate gate.
 *
 * Every scenario is driven through the injectable test wall clock
 * (setTestWallClock), so there is no real clock, no audio device and no
 * engine here — each case has an exact expected answer.
 *
 * Contract pinned (clock/TimeSource.h, clock/TimeSource.cpp):
 *   - in lockstep (audio time and wall time advancing together) the clock
 *     tracks the wall clock exactly;
 *   - a residual under the slew bound re-converges at the IIR rate
 *     (kDriftIirGain per update); one over it is taken up at the bound, a
 *     block at a time (kClockSlewPerBlock) — the clock is a ruler, and what
 *     is scheduled on it stays where its sender put it — and the clock never
 *     runs backwards; a wall clock that went forward is followed the same
 *     way, at the bound: a gap is the host's to see (clockwork_audio_back),
 *     and it anchors the clock afresh (resetAudioThreadTime);
 *   - a wall clock that went back never takes the clock back: the clock
 *     eases toward it at the bound, still advancing with its samples;
 *   - sample accounting mismatched with the wall rate beyond the bound is a
 *     fault the clock shows rather than hides: it stays with its samples and
 *     the drift reading grows without limit;
 *   - resetAudioThreadTime re-anchors and zeroes accumulated error at once;
 *   - freewheel mode ignores the wall clock entirely and derives NTP purely
 *     from sample position;
 *   - DriftLogGate reports a genuine step immediately, then a persisting
 *     condition at most once a minute.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "clock/TimeSource.h"
#include "clock/clock_math.h"   // kDriftIirGain

#include <cmath>

using Catch::Approx;

namespace {

constexpr double kRate  = 48000.0;
constexpr double kBlock = 128.0;
constexpr double kDt    = kBlock / kRate;       // seconds of audio per update
constexpr double kAnchorNTP = 3900000000.0;     // arbitrary plausible NTP instant
// The clock keeps its base as a double at NTP magnitude, whose resolution is
// ~0.48 µs: a block's pull moves it by a whole number of those (2 µs is four,
// so the bound takes up 1.9 µs a block), and a pull under half of one moves it
// not at all — the IIR cannot close a residual below ~24 µs (half a quantum
// over the gain). About a sample at 48 kHz: the wall alignment's floor, not
// the ruler's, which is the sample counter.
constexpr double kBaseQuantum = 4.8e-7;
constexpr double kAlignmentFloor = 0.5 * kBaseQuantum / clockwork::kDriftIirGain;

// The wall is kept as the sample time plus how far it has run ahead, and made
// afresh each step: a double at NTP magnitude (3.9e9) has a resolution of
// ~0.5 µs, and a wall accumulated by adding a block's length at a time would
// walk away from the sample time by a fraction of a microsecond a step — a
// ramp the clock would follow, and a test at the microsecond would read as
// the clock's. Readings here are within half a microsecond of what they
// should be; assertions at the microsecond allow that.
constexpr double kUlp = 1e-6;

struct ClockRig {
    TimeSource ts;
    double samples = 0.0;
    double ahead   = 0.0;          // the wall ahead of the sample time
    double wall    = kAnchorNTP;   // the reading the clock was last handed

    ClockRig() {
        ts.setTestWallClock(wall);
        ts.resetAudioThreadTime(samples, kRate, wall);
        settle();
    }

    double trueWall() const { return kAnchorNTP + samples / kRate + ahead; }

    // One audio callback: samples and wall advance (independently
    // controllable), then the IIR runs. Returns the clock's NTP answer.
    double step(double sampleAdvance = kBlock, double wallAdvance = kDt) {
        samples += sampleAdvance;
        ahead   += wallAdvance - sampleAdvance / kRate;
        return reading(trueWall());
    }
    // One callback whose wall reading is `noise` off the true wall (a host
    // reading Date.now() in a render burst).
    double stepReading(double noise) {
        samples += kBlock;
        return reading(trueWall() + noise);
    }
    double reading(double w) {
        wall = w;
        ts.setTestWallClock(wall);
        return ts.updateAudioThreadNTP(samples, kRate, wall);
    }
    // The wall moves on by `secs` with no block rendered (a stall; negative: a wall clock set back).
    void stall(double secs) { ahead += secs; wall = trueWall(); ts.setTestWallClock(wall); }
    // Anchor afresh on a reading (the clock reads the rig's wall, so it is set first).
    void anchorOn(double w) { wall = w; ts.setTestWallClock(wall); ts.resetAudioThreadTime(samples, kRate, wall); }
    // Past an anchor's settling window (kAnchorSettleBlocks), on exact readings.
    void settle() { for (uint32_t i = 0; i < clockwork::kAnchorSettleBlocks; ++i) step(); }
};

} // namespace

TEST_CASE("TimeSource: lockstep steady state tracks wall clock exactly",
          "[TimeSource]") {
    ClockRig rig;
    for (int i = 0; i < 2000; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp == Approx(rig.wall).margin(1e-6));
    }
    REQUIRE(rig.ts.now() == Approx(rig.wall).margin(1e-6));
}

TEST_CASE("TimeSource: a residual under the slew bound converges at the IIR rate, "
          "monotonically", "[TimeSource]") {
    ClockRig rig;
    for (int i = 0; i < 500; ++i) rig.step();
    const uint32_t anchors = rig.ts.anchors();

    // A small stall: the wall advances 150 µs while the audio thread
    // delivers nothing. 1% of it is under the bound, so the IIR alone
    // takes it up.
    const double stall = 0.00015;
    REQUIRE(stall * clockwork::kDriftIirGain < clockwork::kClockSlewPerBlock);
    rig.stall(stall);

    double prev = rig.ts.now();
    double firstGapError = 0.0;
    for (int i = 0; i < 1500; ++i) {
        const double ntp = rig.step();
        if (i == 0) firstGapError = rig.wall - ntp;
        REQUIRE(ntp >= prev);
        prev = ntp;
    }
    // The first answer after it is still ~150 µs behind the wall (1% a
    // step, no snap) ...
    REQUIRE(firstGapError == Approx(stall * (1.0 - clockwork::kDriftIirGain)).margin(kUlp));
    // ... and after n steps the residual is 0.99^n of it, down to the
    // alignment floor.
    const double expectedResidual = stall * std::pow(1.0 - clockwork::kDriftIirGain, 1500);
    REQUIRE(rig.wall - rig.ts.now() >= expectedResidual - kUlp);
    REQUIRE(rig.wall - rig.ts.now() <= expectedResidual + kAlignmentFloor + kUlp);
    REQUIRE(rig.ts.anchors() == anchors);   // converged, not anchored afresh
}

TEST_CASE("TimeSource: a residual over the slew bound is taken up at the bound, a block at a time, "
          "so what is scheduled on the clock stays where it was put", "[TimeSource]") {
    // The clock is the ruler everything is scheduled on: a bundle placed
    // against it must land where its sender worked it out, whatever the
    // wall is doing. A stall of 50 ms (under kClockStepSeconds: no step)
    // leaves the clock 50 ms behind; the IIR would pull it 500 µs in the
    // next block, and a bundle 50 ms out would land a quarter of a
    // millisecond early.
    ClockRig rig;
    for (int i = 0; i < 500; ++i) rig.step();
    const uint32_t anchors = rig.ts.anchors();
    const double stall = 0.05;
    REQUIRE(stall < clockwork::kClockStepSeconds);
    rig.stall(stall);

    double prev = rig.ts.now();
    for (int i = 0; i < 1000; ++i) {
        const double ntp = rig.step();
        // Each block moves the ruler by its samples, and against them by no
        // more than the bound.
        REQUIRE(ntp - prev >= kDt - kUlp);
        REQUIRE(ntp - prev <= kDt + clockwork::kClockSlewPerBlock + kUlp);
        prev = ntp;
    }
    // 1000 blocks on: 2 ms of the 50 taken up (1.9, in whole base quanta), no more.
    const double takenUp = stall - (rig.wall - rig.ts.now());
    REQUIRE(takenUp <= 1000 * clockwork::kClockSlewPerBlock + kUlp);
    REQUIRE(takenUp >= 1000 * (clockwork::kClockSlewPerBlock - 0.5 * kBaseQuantum) - kUlp);
    REQUIRE(rig.ts.anchors() == anchors);
}

TEST_CASE("TimeSource: an anchor settles on the median of its first readings", "[TimeSource]") {
    // An anchor is one reading, and where the host reads Date.now() in
    // render bursts one reading is out by up to a burst. The clock holds the
    // reading for kAnchorSettleBlocks, then moves once to the median of the
    // readings meanwhile — the burst's middle, within a millisecond of the
    // truth — and a callback that woke late (one reading far out) is one
    // vote. It counts as the one anchor, not a second.
    ClockRig rig;
    // Readings in bursts of four: the first exact, the next three each a
    // block stale (the thread read the wall once and rendered four).
    constexpr double burst[4] = { 0.0, -kDt, -2 * kDt, -3 * kDt };
    const double medianNoise = -kDt;   // 16 of each: the 33rd smallest is -kDt
    // Anchor on a bad reading, 5 ms behind the truth (a stale reading), after
    // a 1 s stall so the truth is well ahead of the clock's last published
    // time; otherwise the floor (never below the last published time) would
    // mask the bad reading.
    rig.stall(1.0);
    rig.anchorOn(rig.trueWall() - 0.005);
    const uint32_t anchors = rig.ts.anchors();

    uint32_t k = 0;
    double ntp = 0.0;
    for (; k + 1 < clockwork::kAnchorSettleBlocks; ++k) {
        ntp = k == 10 ? rig.stepReading(0.030) : rig.stepReading(burst[k % 4]);   // one woke 30 ms late
        REQUIRE(ntp - rig.trueWall() == Approx(-0.005).margin(kUlp));   // held on the reading, nothing moved
    }
    ntp = rig.stepReading(burst[k % 4]);   // the window's last: the clock moves once, on to the readings' median
    REQUIRE(ntp - rig.trueWall() == Approx(medianNoise).margin(kUlp));
    REQUIRE(rig.ts.anchors() == anchors);
    // Settled: the bound holds it from here, whatever the readings do.
    double prev = ntp;
    for (int i = 0; i < 200; ++i) {
        ntp = rig.stepReading(burst[i % 4]);
        REQUIRE(ntp - prev >= kDt - clockwork::kClockSlewPerBlock - kUlp);
        REQUIRE(ntp - prev <= kDt + clockwork::kClockSlewPerBlock + kUlp);
        prev = ntp;
    }
}

TEST_CASE("TimeSource: an anchor that read late is not moved back by the settle; it is taken up by "
          "slowing, and the clock is on the wall within a few dozen blocks", "[TimeSource]") {
    // The clock anchored ahead of the wall (a reading late in a render
    // burst; a context filling its output buffer at start): the readings
    // that follow say so, and the median would move the clock back. Time
    // never goes back: the settle takes the correction up by slowing,
    // kAnchorTakeUpShare of a block a block, until it is on the wall.
    ClockRig rig;
    rig.anchorOn(rig.trueWall() + 0.0055);
    const uint32_t anchors = rig.ts.anchors();
    const double anchored = rig.ts.now();
    double prev = anchored;
    for (uint32_t i = 0; i < clockwork::kAnchorSettleBlocks; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp - prev >= kDt - kUlp);   // held on the reading through the settle
        prev = ntp;
    }
    REQUIRE(prev - rig.wall == Approx(0.0055).margin(kUlp));
    // Taken up: never back, no slower than the share says, and done in the
    // blocks the share says (5.5 ms at an eighth of 2.67 ms a block: 17).
    const uint32_t blocks = static_cast<uint32_t>(std::ceil(0.0055 / (clockwork::kAnchorTakeUpShare * kDt)));
    for (uint32_t i = 0; i < blocks; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp - prev >= kDt * (1.0 - clockwork::kAnchorTakeUpShare) - kUlp);
        REQUIRE(ntp - prev <= kDt + kUlp);
        prev = ntp;
    }
    REQUIRE(prev - rig.wall == Approx(0.0).margin(2 * kUlp));
    // And on the wall from here, at the bound.
    for (int i = 0; i < 100; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp - prev >= kDt - clockwork::kClockSlewPerBlock - kUlp);
        REQUIRE(ntp - prev <= kDt + clockwork::kClockSlewPerBlock + kUlp);
        prev = ntp;
    }
    REQUIRE(prev - rig.wall == Approx(0.0).margin(kAlignmentFloor + kUlp));
    REQUIRE(rig.ts.anchors() == anchors);
}

TEST_CASE("TimeSource: a wall clock that went forward is followed no faster than the bound; "
          "a gap is the host's to say (resetAudioThreadTime)", "[TimeSource]") {
    // The wall stepped forward under a clock whose samples kept coming (a
    // system clock set by hand): not a gap. The clock is the ruler, and
    // follows at the bound a block, as it does a wall set back. The audio
    // away is measured by the host against the audio it rendered
    // (clockwork_audio_back), which anchors the clock afresh at the block
    // back; the clock itself never steps on a wall reading.
    ClockRig rig;
    for (int i = 0; i < 500; ++i) rig.step();
    const uint32_t anchors = rig.ts.anchors();

    rig.stall(1.0);
    double prev = rig.ts.now();
    for (int i = 0; i < 300; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp - prev >= kDt - kUlp);
        REQUIRE(ntp - prev <= kDt + clockwork::kClockSlewPerBlock + kUlp);
        prev = ntp;
    }
    REQUIRE(rig.ts.anchors() == anchors);
    REQUIRE(rig.wall - prev > 0.99);   // still nearly the whole second behind the wall

    // The host says the audio was away: anchored afresh, on the wall at once.
    rig.anchorOn(rig.trueWall());
    REQUIRE(rig.ts.now() == Approx(rig.wall).margin(1e-6));
    REQUIRE(rig.ts.anchors() == anchors + 1);
}

TEST_CASE("TimeSource: a wall clock that went back never takes the clock back", "[TimeSource]") {
    // The wall stepped back (a system clock set by hand): the clock keeps
    // advancing with its samples, giving back the bound a block, until the
    // wall has caught up — a reader never sees time go back, whatever the
    // wall did.
    ClockRig rig;
    for (int i = 0; i < 500; ++i) rig.step();

    rig.stall(-1.0);
    double prev = rig.ts.now();
    const double from = prev;
    for (int i = 0; i < 300; ++i) {
        const double ntp = rig.step();
        REQUIRE(ntp - prev >= kDt - clockwork::kClockSlewPerBlock - kUlp);
        REQUIRE(ntp - prev <= kDt + kUlp);
        prev = ntp;
    }
    // 300 blocks on: the clock has moved on by its samples less 300 bounds (in whole base quanta).
    REQUIRE(prev - from >= 300 * kDt - 300 * clockwork::kClockSlewPerBlock - kUlp);
    REQUIRE(prev - from <= 300 * kDt - 300 * (clockwork::kClockSlewPerBlock - 0.5 * kBaseQuantum) + kUlp);
    REQUIRE(rig.wall < prev);
}

TEST_CASE("TimeSource: a rate mismatch beyond the slew bound is shown, not hidden", "[TimeSource]") {
    // 10ms callbacks whose sample accounting runs 10% fast against wall time
    // (what a stale rate assumption across a device change looks like): a
    // fault, far beyond any crystal's drift. The clock stays with its samples
    // — the bound lets it give back only kClockSlewPerBlock an update — and
    // runs ahead of the wall by nearly the whole mismatch, which the drift
    // reading shows growing without limit. Correct sample accounting across
    // device changes is what keeps the injection term at zero; a clock that
    // followed the wall here would hide the fault in a steady 100 ms error.
    ClockRig rig;
    const double devDt      = 0.010;            // 10ms callbacks
    const double devSamples = kRate * devDt;    // nominal samples per callback

    double ntp = 0.0;
    for (int i = 0; i < 6000; ++i)              // 60s simulated
        ntp = rig.step(devSamples * 1.1, devDt);

    const double ahead = ntp - rig.wall;        // clock AHEAD of wall
    const double injected = 6000 * 0.1 * devDt; // 6 s of extra samples
    REQUIRE(ahead == Approx(injected - 6000 * clockwork::kClockSlewPerBlock).margin(0.001));
    REQUIRE(rig.ts.lastDriftUs() > 1000000);    // the clock over a second ahead of the wall, and growing
}

TEST_CASE("TimeSource: re-anchor (resetAudioThreadTime) zeroes accumulated "
          "error immediately", "[TimeSource]") {
    ClockRig rig;
    for (int i = 0; i < 300; ++i) rig.step(kBlock * 1.1, kDt);  // build error
    REQUIRE(std::abs(rig.ts.now() - rig.wall) > 0.005);

    rig.ts.resetAudioThreadTime(rig.samples, kRate, rig.wall);
    const double ntp = rig.step();
    REQUIRE(ntp == Approx(rig.wall).margin(1e-4));
}

TEST_CASE("TimeSource: freewheel ignores the wall clock entirely",
          "[TimeSource]") {
    ClockRig rig;
    rig.ts.setFreewheelClock(true);
    const double anchor = rig.wall;  // base captured by the last reset

    for (int i = 0; i < 200; ++i) {
        // Wall clock goes haywire: jumps a full second per block.
        const double ntp = rig.step(kBlock, 1.0);
        REQUIRE(ntp == Approx(anchor + rig.samples / kRate).margin(1e-9));
    }
}

TEST_CASE("TimeSource: the drift line stays quiet while an anchor settles, and is "
          "said once it has", "[TimeSource][DriftLog]") {
    ClockRig rig;
    REQUIRE(rig.ts.driftReports() == 0);
    // An anchor on a reading 5.5 ms early (a device start's first callback):
    // every exact reading after it is over the diagnostic's threshold, but
    // that is what the settling window is for, not a line.
    rig.anchorOn(rig.trueWall() - 0.0055);
    rig.settle();
    REQUIRE(rig.ts.driftReports() == 0);
    // Settled on the median, the clock is on the wall: still nothing to say.
    for (int i = 0; i < 100; ++i) rig.step();
    REQUIRE(rig.ts.driftReports() == 0);
    REQUIRE(rig.ts.now() == Approx(rig.wall).margin(kAlignmentFloor));
    // A real excursion once settled is said at once.
    rig.stall(0.006);
    rig.step();
    REQUIRE(rig.ts.driftReports() == 1);
}

// ── Drift-log rate gate ──────────────────────────────────────────────────────
// Steady-state jitter on VMs and on shared-mode Windows audio hovers right at
// the drift threshold, so a once-per-second rate limit still logs forever
// (thousands of lines per session). The gate reports a genuine step
// immediately, then a persisting condition at most once a minute.

TEST_CASE("DriftLogGate: quiet clock never logs", "[DriftLog]") {
    clockwork::DriftLogGate gate;
    for (int i = 0; i < 1000; ++i)
        REQUIRE_FALSE(gate.offer(0.004, 1000.0 + i * 0.02));
    REQUIRE(gate.reports() == 0);
}

TEST_CASE("DriftLogGate: first excursion logs immediately", "[DriftLog]") {
    clockwork::DriftLogGate gate;
    REQUIRE(gate.offer(0.0062, 5000.0));
    REQUIRE(gate.reports() == 1);
}

TEST_CASE("DriftLogGate: sustained jitter is said once a minute", "[DriftLog]") {
    clockwork::DriftLogGate gate;
    // Two full minutes of 50Hz callbacks all hovering over threshold, either
    // side of it (6001 ticks spans t=1000..1120 inclusive, crossing both
    // minute marks).
    for (int i = 0; i < 6001; ++i) {
        const double now = 1000.0 + i * 0.02;
        gate.offer((i % 2 == 0) ? 0.006 : -0.008, now);
    }
    REQUIRE(gate.reports() == 3);     // immediate + one per following minute
}

TEST_CASE("DriftLogGate: a step long after the last report logs immediately",
          "[DriftLog]") {
    clockwork::DriftLogGate gate;
    REQUIRE(gate.offer(0.010, 1000.0));
    // Quiet hour.
    for (int i = 0; i < 100; ++i)
        REQUIRE_FALSE(gate.offer(0.001, 1100.0 + i));
    // Fresh step: reported at once.
    REQUIRE(gate.offer(-0.020, 4600.0));
    REQUIRE(gate.reports() == 2);
}
