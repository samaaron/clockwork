// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// A channel's reading of the engine's clock never goes back. In postMessage
// mode the reading is the audio thread's last word carried on by the page's
// own clock, and the next word can land behind that: the reading holds.
import { test } from "node:test";
import assert from "node:assert/strict";
import { OscChannel } from "../js/lib/osc_channel.js";

test("postMessage: now() never goes back, whatever the audio thread's next word says", () => {
  let t = 1000;
  const realNow = performance.now;
  performance.now = () => t;
  try {
    const port = {};
    const ch = OscChannel.createPostMessage({ port });
    assert.equal(ch.now(), 0);                                  // nothing heard yet
    port.onmessage({ data: { type: "clock", ntp: 100.0 } });
    t = 1010;
    const a = ch.now();
    assert.ok(Math.abs(a - 100.010) < 1e-9, `carried on by the page's clock: ${a}`);
    port.onmessage({ data: { type: "clock", ntp: 100.005 } }); // the word lands behind the reading
    const b = ch.now();
    assert.ok(b >= a, `went back: ${a} -> ${b}`);
    t = 1020;
    const c = ch.now();
    assert.ok(c >= b, `went back: ${b} -> ${c}`);
    assert.ok(c > a, "and moves on");
  } finally {
    performance.now = realNow;
  }
});
