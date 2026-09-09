// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * The DSP profile — the client's half of the boundary.
 *
 * Clockwork carries no DSP, but the client still has to know a couple of the
 * DSP's *words*. Two things force it:
 *
 *   1. sync() waits on a barrier, so it must know which verb asks and which
 *      reply answers.
 *   2. Some verbs must be refused client-side because the engine would take
 *      them literally (a quit that kills the audio thread, say).
 *
 * Neither is synthesis. It is vocabulary, and it differs between DSPs:
 *
 *              scsynth              a guest under its own prefix
 *   sync       /sync → /synced      /clockwork/sync → /clockwork/synced
 *   refuse     —                    /clockwork/quit
 *
 * So it arrives as data. A DSP supplies one of these; clockwork reads it
 * and holds no opinion about which engine is underneath.
 *
 * DEFINITIONS ARE NOT DECLARED HERE. A reload puts back whatever the product
 * was holding, through restoreClientState(), which is the path for every other
 * category of state — so no guest has to declare a definition format to get it.
 *
 * METRICS ARE DECLARED THE SAME WAY. A guest keeps counters clockwork cannot
 * name, so it declares them rather than occupying fixed offsets here:
 *
 *   metrics: {
 *     bufferPoolGrowthCount: { slot: 3, type: "u32",
 *                              description: "times the pool has grown" },
 *   }
 *
 * `slot` is an index from 0 into the reserved guest range, not an absolute
 * offset — clockwork may move the range and a guest should not care. The
 * client supplies values for these names from `clientMetrics()`, and a name
 * that was never declared is dropped rather than written somewhere arbitrary.
 *
 * A guest may also supply `metricsPanels` — the same shape the metrics
 * schema's layout uses — so its numbers appear in the metrics UI beside
 * clockwork's. A panel is a claim about what a guest HAS, so it belongs with
 * the declaration rather than hardcoded in a shared layout.
 *
 */

import { GUEST_METRICS_COUNT } from "./metrics_offsets.js";

/**
 * @typedef {object} DspProfile
 * @property {string}   syncVerb     OSC address that asks for a barrier.
 * @property {string}   syncedVerb   OSC address the engine replies with.
 * @property {Record<string,string>} blockedVerbs
 *           address -> the message the client throws instead of sending.
 */

/**
 * The null profile: correct for clockwork with no DSP attached, and the
 * default so that nothing here fails merely because a profile is missing.
 * Sync is unavailable and nothing is blocked.
 * @type {DspProfile}
 */
export const NO_DSP = Object.freeze({
  syncVerb: null,
  syncedVerb: null,
  blockedVerbs: Object.freeze({}),
  metrics: Object.freeze({}),
  metricsPanels: Object.freeze([]),
});

/**
 * Validate a profile at the boundary, so a malformed one fails where it is
 * supplied rather than at the first reload — which is hours later and looks
 * like a caching bug.
 * @param {Partial<DspProfile>|null|undefined} p
 * @returns {DspProfile}
 */
export function dspProfile(p) {
  if (!p) return NO_DSP;
  const str = (k) => {
    const v = p[k];
    if (v == null) return null;
    if (typeof v !== "string" || !v.startsWith("/")) {
      throw new TypeError(`dsp profile: ${k} must be an OSC address, got ${JSON.stringify(v)}`);
    }
    return v;
  };
  const syncVerb = str("syncVerb");
  const syncedVerb = str("syncedVerb");
  if (Boolean(syncVerb) !== Boolean(syncedVerb)) {
    throw new TypeError("dsp profile: syncVerb and syncedVerb come as a pair");
  }
  /*
   * Metrics are checked here rather than discovered as a wrong number later.
   * A slot outside the range, or two metrics sharing one, would silently
   * write over another guest metric or over nothing at all — and a metric
   * reading zero looks exactly like a metric with nothing to report.
   */
  const metrics = {};
  const takenSlots = new Map();
  for (const [name, def] of Object.entries(p.metrics ?? {})) {
    if (!def || !Number.isInteger(def.slot)) {
      throw new TypeError(`dsp profile: metric ${name} needs an integer slot`);
    }
    if (def.slot < 0 || def.slot >= GUEST_METRICS_COUNT) {
      throw new RangeError(
        `dsp profile: metric ${name} slot ${def.slot} is outside the guest range `
        + `(0..${GUEST_METRICS_COUNT - 1})`);
    }
    if (takenSlots.has(def.slot)) {
      throw new TypeError(
        `dsp profile: metrics ${takenSlots.get(def.slot)} and ${name} both claim slot ${def.slot}`);
    }
    takenSlots.set(def.slot, name);
    metrics[name] = Object.freeze({ ...def });
  }

  return Object.freeze({
    syncVerb,
    syncedVerb,
    blockedVerbs: Object.freeze({ ...(p.blockedVerbs ?? {}) }),
    metrics: Object.freeze(metrics),
    metricsPanels: Object.freeze([...(p.metricsPanels ?? [])]),
  });
}
