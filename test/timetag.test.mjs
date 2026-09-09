// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//
// The OSC-timetag → DOMHighResTimeStamp conversion, on its own.
//
// Worth testing in isolation because it crosses two epochs and a fixed-point
// format, and every way of getting it wrong produces a plausible-looking
// number: a note in 1900, a note in 2106, or a note 70 years late.
import { test } from "node:test";
import assert from "node:assert/strict";
import { timetagToPerfMs, unixMsToTimetag, NTP_EPOCH_OFFSET } from "../js/lib/timetag.js";

test("the immediate sentinels are not a time", () => {
  assert.equal(timetagToPerfMs(0n, 0), undefined);
  assert.equal(timetagToPerfMs(1n, 0), undefined);
});

test("a timetag converts to the browser's clock", () => {
  // 1 January 2020, 00:00:00 UTC.
  const unixMs = Date.UTC(2020, 0, 1);
  const origin = unixMs - 5000;               // the page loaded 5s earlier
  const at = timetagToPerfMs(unixMsToTimetag(unixMs), origin);
  assert.ok(Math.abs(at - 5000) < 1, `expected ~5000ms after origin, got ${at}`);
});

test("a round trip through the timetag keeps the millisecond", () => {
  for (const unixMs of [0, 1, 1_000, 1_600_000_000_123, Date.now()]) {
    const back = timetagToPerfMs(unixMsToTimetag(unixMs), 0);
    assert.ok(Math.abs(back - unixMs) < 1,
      `${unixMs} -> ${back}, drifted by ${Math.abs(back - unixMs)}ms`);
  }
});

test("the epoch offset is the NTP one, not the Unix one", () => {
  // The single most likely error: forgetting the 70 years. A timetag whose
  // seconds field is exactly the offset is the Unix epoch itself.
  assert.equal(NTP_EPOCH_OFFSET, 2208988800);
  const at = timetagToPerfMs(BigInt(NTP_EPOCH_OFFSET) << 32n, 0);
  assert.ok(Math.abs(at) < 1, `the Unix epoch should be 0ms, got ${at}`);
});

test("the top bit being set is not a negative time", () => {
  // A present-day timetag has its top bit set. Read as signed anywhere in the
  // chain it goes negative, which is the trap this tree has hit four times.
  const now = unixMsToTimetag(Date.now());
  assert.ok(now > (1n << 63n), "a present-day timetag has its top bit set");
  const at = timetagToPerfMs(now, 0);
  assert.ok(at > 0, `must not come out negative, got ${at}`);
});
