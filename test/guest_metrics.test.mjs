// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// A guest's metrics declaration is checked where it is supplied.
import { test } from "node:test";
import assert from "node:assert/strict";
import { guestMetrics } from "../js/lib/guest_metrics.js";
import { GUEST_METRICS_COUNT } from "../js/lib/metrics_offsets.js";

test("a good declaration is kept, frozen", () => {
  const m = guestMetrics({ growth: { slot: 3, type: "u32", description: "times the pool grew" } });
  assert.equal(m.growth.slot, 3);
  assert.ok(Object.isFrozen(m) && Object.isFrozen(m.growth));
  assert.deepEqual(guestMetrics(undefined), {});
});

test("a slot outside the guest range, a missing slot, or two names on one slot are refused", () => {
  assert.throws(() => guestMetrics({ a: { slot: GUEST_METRICS_COUNT } }), /outside the guest range/);
  assert.throws(() => guestMetrics({ a: { slot: -1 } }), /outside the guest range/);
  assert.throws(() => guestMetrics({ a: {} }), /needs an integer slot/);
  assert.throws(() => guestMetrics({ a: { slot: 1 }, b: { slot: 1 } }), /both claim slot 1/);
});
