// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// js/lib/clock_math.js is the twin of src/clock/clock_math.h: the tempo grid
// (beat = (t − origin) · bpm / 60) and the re-anchor that keeps a beat still
// across a tempo change. The C++ side is pinned by test_clock_state.cpp; these
// pin the JS side to the same numbers so the two cannot drift.
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  beatAt, timeAtBeat, originFor, retempoOrigin, wrapPhase,
} from "../js/lib/clock_math.js";

const close = (a, b, msg) => assert.ok(Math.abs(a - b) < 1e-9, `${msg}: ${a} vs ${b}`);

test("beatAt / timeAtBeat / originFor agree with each other", () => {
  close(beatAt(1010, 1000, 120), 20, "ten seconds at 120 is 20 beats");
  close(timeAtBeat(20, 1000, 120), 1010, "beat 20 is at t=1010");
  close(originFor(8, 1004, 120), 1000, "beat 8 at 1004 puts the origin at 1000");
  close(beatAt(timeAtBeat(7.25, 1000, 133), 1000, 133), 7.25, "round trip");
});

test("retempoOrigin holds the beat that is playing", () => {
  const origin = 1000, now = 1010;
  const held = beatAt(now, origin, 120);
  for (const bpm of [240, 60, 120, 133.7]) {
    close(beatAt(now, retempoOrigin(origin, 120, bpm, now), bpm), held,
      `beat held across 120 -> ${bpm}`);
  }
  // A beat later at the new tempo is a beat later.
  close(beatAt(now + 1, retempoOrigin(origin, 120, 240, now), 240), held + 4,
    "four beats per second at 240");
});

test("retempoOrigin leaves an unanchored or nonsensical grid alone", () => {
  assert.equal(retempoOrigin(0, 120, 240, 1010), 0);
  assert.equal(retempoOrigin(1000, 0, 240, 1010), 1000);
  assert.equal(retempoOrigin(1000, NaN, 240, 1010), 1000);
});

test("wrapPhase is non-negative and zero for a non-positive quantum", () => {
  close(wrapPhase(5.5, 4), 1.5, "5.5 in 4");
  close(wrapPhase(-0.5, 4), 3.5, "-0.5 in 4");
  assert.equal(wrapPhase(5.5, 0), 0);
});
