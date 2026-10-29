// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/**
 * A guest's metrics, declared to the client.
 *
 * A guest keeps counters clockwork cannot name, so it declares them rather
 * than occupying fixed offsets in clockwork's schema:
 *
 *   new Clockwork({ guestMetrics: {
 *     bufferPoolGrowthCount: { slot: 3, type: "u32",
 *                              description: "times the pool has grown" },
 *   }, guestMetricsPanels: [ ... ] })
 *
 * `slot` is an index from 0 into the reserved guest range, not an absolute
 * offset — clockwork may move the range and a guest should not care. The
 * client supplies values for these names from `clientMetrics()`, and a name
 * that was never declared is dropped rather than written somewhere arbitrary.
 * `guestMetricsPanels` is the shape the metrics schema's layout uses, so the
 * guest's numbers appear in the metrics UI beside clockwork's.
 *
 * Nothing else is declared here: the sync barrier is clockwork's own verb,
 * and refusing an engine's dangerous verbs, or naming the address refusals
 * come on, is the product's business in its own send()/request().
 */
import { GUEST_METRICS_COUNT } from "./metrics_offsets.js";

/**
 * Validate a declaration at the boundary, so a wrong one fails where it is
 * supplied rather than as a wrong number later. A slot outside the range,
 * or two metrics sharing one, would silently write over another guest
 * metric or over nothing at all — and a metric reading zero looks exactly
 * like a metric with nothing to report.
 * @param {Record<string, {slot: number}>|null|undefined} declared
 * @returns {Readonly<Record<string, object>>}
 */
export function guestMetrics(declared) {
  const metrics = {};
  const takenSlots = new Map();
  for (const [name, def] of Object.entries(declared ?? {})) {
    if (!def || !Number.isInteger(def.slot)) {
      throw new TypeError(`guestMetrics: metric ${name} needs an integer slot`);
    }
    if (def.slot < 0 || def.slot >= GUEST_METRICS_COUNT) {
      throw new RangeError(
        `guestMetrics: metric ${name} slot ${def.slot} is outside the guest range `
        + `(0..${GUEST_METRICS_COUNT - 1})`);
    }
    if (takenSlots.has(def.slot)) {
      throw new TypeError(
        `guestMetrics: metrics ${takenSlots.get(def.slot)} and ${name} both claim slot ${def.slot}`);
    }
    takenSlots.set(def.slot, name);
    metrics[name] = Object.freeze({ ...def });
  }
  return Object.freeze(metrics);
}
