// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron

/**
 * clock_math.js — the tempo-grid arithmetic, once, on the JS side.
 *
 * Twin of src/clock/clock_math.h: beat = (t − origin) · bpm / 60 and its
 * inverses, plus the re-anchor that keeps a beat still across a tempo
 * change. test/clock_math.test.mjs pins these to the numbers
 * test_clock_state.cpp pins the C++ to.
 */

export function beatAt(t, origin, bpm) {
  return (t - origin) * bpm / 60.0;
}

export function timeAtBeat(beat, origin, bpm) {
  return origin + beat * 60.0 / bpm;
}

/** The origin that puts `beat` at time `t`. */
export function originFor(beat, t, bpm) {
  return t - beat * 60.0 / bpm;
}

/**
 * A tempo change must not move the beat that is playing. This is the origin
 * that holds the beat playing at `now` under `oldBpm` still at `now` under
 * `newBpm`. An unanchored grid (origin 0) or a nonsensical old tempo is
 * returned untouched — see clock_math.h retempoOrigin for the history.
 */
export function retempoOrigin(origin, oldBpm, newBpm, now) {
  if (origin === 0 || !(oldBpm >= 1)) return origin;
  return originFor(beatAt(now, origin, oldBpm), now, newBpm);
}

/** Non-negative phase of `beat` within `quantum` (0 when quantum <= 0). */
export function wrapPhase(beat, quantum) {
  if (!(quantum > 0)) return 0;
  let p = beat % quantum;
  if (p < 0) p += quantum;
  return p;
}
