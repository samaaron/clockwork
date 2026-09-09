// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// ClockworkClock (js/lib/clockwork_clock.js) over a SharedArrayBuffer, with no browser:
// a tempo change must not move the beat that is playing. The C++ session
// writers learned this on 2026-09-01 (test_clock_tempo_change.cpp); the JS
// writer, which is the one a web page's setBpm actually reaches, had the same
// bug and no test.
import { test } from "node:test";
import assert from "node:assert/strict";
import { ClockworkClock } from "../js/lib/clockwork_clock.js";

// The arena constants a headless clock needs, laid out back to back.
const CONSTS = {
  NTP_START_TIME_START: 0,
  DRIFT_OFFSET_START: 8,
  GLOBAL_OFFSET_START: 12,
  CLOCK_STATE_START: 16,
  CLOCK_STATE_SIZE: 40,
};

function headlessClock() {
  const sab = new SharedArrayBuffer(64);
  const clock = new ClockworkClock({ mode: "sab" });
  clock.initSharedViews(sab, 0, CONSTS);
  return clock;
}

const close = (a, b, msg) => assert.ok(Math.abs(a - b) < 1e-9, `${msg}: ${a} vs ${b}`);

test("setBpm keeps the current beat where it is", () => {
  const clock = headlessClock();
  clock.setBpm(120);
  clock.requestBeatAtTime(0, 1000, 4);           // beat 0 at t=1000
  const now = 1010;                              // beat 20
  close(clock.beatAtTime(now, 4), 20, "twenty beats in");

  clock.setBpm(240, 0, now);
  assert.equal(clock.getBpm(), 240);
  close(clock.beatAtTime(now, 4), 20, "raising the tempo held the beat");
  close(clock.beatAtTime(now + 1, 4), 24, "and it runs at the new rate");

  clock.setBpm(60, 0, now);
  close(clock.beatAtTime(now, 4), 20, "lowering the tempo held the beat");
  close(clock.timeAtBeat(21, 4), now + 1, "next beat is a second away");
});

test("setBpm before the grid is placed only sets the tempo", () => {
  const clock = headlessClock();
  clock.setBpm(90, 0, 5000);
  assert.equal(clock.getBpm(), 90);
  assert.equal(clock.getBeatOriginNtp(), 0);
});

test("setBpm clamps a nonsense tempo without poisoning the grid", () => {
  const clock = headlessClock();
  clock.setBpm(120);
  clock.requestBeatAtTime(0, 1000, 4);
  clock.setBpm(0, 0, 1010);
  assert.equal(clock.getBpm(), 1);
  assert.ok(Number.isFinite(clock.getBeatOriginNtp()));
  close(clock.beatAtTime(1010, 4), 20, "beat held through the clamp");
});

test("the meter is 4/4 until set, and a set does not move the grid", () => {
  const clock = headlessClock();
  assert.deepEqual(clock.getMeter(), { num: 4, den: 4 });
  clock.setBpm(120);
  clock.requestBeatAtTime(0, 1000, 4);
  assert.equal(clock.setMeter(7, 8), true);
  assert.deepEqual(clock.getMeter(), { num: 7, den: 8 });
  close(clock.beatAtTime(1010, 4), 20, "the beat is where it was");
  assert.equal(clock.getBpm(), 120);
});

test("a meter that is not a meter is refused and the old one stands", () => {
  const clock = headlessClock();
  clock.setMeter(3, 4);
  for (const [num, den] of [[0, 4], [-1, 4], [4, 3], [4, 0], [4, 64], [2.5, 4], [NaN, 4]])
    assert.equal(clock.setMeter(num, den), false, `${num}/${den}`);
  assert.deepEqual(clock.getMeter(), { num: 3, den: 4 });
});

test("setIsPlaying stamps when the transport changed", () => {
  const clock = headlessClock();
  clock.setIsPlaying(true, 500.5);
  assert.equal(clock.isPlaying(), true);
  assert.equal(clock.getIsPlayingAtNtp(), 500.5);
  clock.setIsPlaying(false, 600.25);
  assert.equal(clock.isPlaying(), false);
  assert.equal(clock.getIsPlayingAtNtp(), 600.25);
});
