// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * metrics_history.js — reading what every counter and gauge did over the
 * last hour (src/metrics_history.h is the writer, and says the layout).
 *
 * Three rings of buckets per metric — a second, ten seconds, a minute each —
 * holding the lowest, highest and last value seen in the bucket. A window is
 * the last n buckets of a level: its lowest and highest, the last value, and
 * the change across it (a counter's count in that time). The windows a page
 * gets: the last second, ten seconds and minute from the first ring, ten
 * minutes from the second, the hour from the third. The layout comes from the
 * arena's table (bufferConstants.METRICS_HISTORY_*), the metrics' slots from
 * the schema: every engine metric that is not a constant, in offset order,
 * as the engine's own table numbers them.
 */
import { METRICS_SCHEMA } from './metrics_schema.js';

const NO_BUCKET = 0xFFFFFFFF;
const HDR_ORIGIN_LO = 0, HDR_ORIGIN_HI = 1, HDR_CURRENT = 2, HDR_FIRST = 5;

export const HISTORY_WINDOWS = Object.freeze([
  { name: 's1',  level: 0, buckets: 1 },
  { name: 's10', level: 0, buckets: 10 },
  { name: 'm1',  level: 0, buckets: 60 },
  { name: 'm10', level: 1, buckets: 60 },
  { name: 'h1',  level: 2, buckets: 60 },
]);

// The metrics with a history and their slots, from the schema: engine words
// (offset under the block's size), not a constant, in offset order.
export function historySlots(schema = METRICS_SCHEMA, metricWords = 80) {
  const slots = [];
  const engine = Object.entries(schema.metrics)
    .map(([key, def]) => ({ key, ...def }))
    .filter((m) => m.offset < metricWords && !m.webOnly)
    .sort((a, b) => a.offset - b.offset);
  for (const m of engine) if (m.type !== 'constant') slots.push({ key: m.key, offset: m.offset, signed: Boolean(m.signed), slot: slots.length });
  return slots;
}

const asValue = (raw, signed) => (signed ? (raw | 0) : raw >>> 0);

// One window: the last `buckets` buckets of `level` for the metric at `slot`.
export function historyWindow(words, bc, level, buckets, slot, signed) {
  const out = { low: 0, high: 0, last: 0, delta: 0, covered: 0 };
  const levels = bc.METRICS_HISTORY_LEVELS, ring = bc.METRICS_HISTORY_BUCKETS, metrics = bc.METRICS_HISTORY_METRICS;
  const perBucket = bc.METRICS_HISTORY_WORDS, headerWords = bc.METRICS_HISTORY_HEADER_BYTES >> 2;
  if (!words || level >= levels || buckets === 0 || slot < 0 || slot >= metrics) return out;
  if (words[HDR_ORIGIN_LO] === 0 && words[HDR_ORIGIN_HI] === 0) return out;
  const first = words[HDR_FIRST + level], current = words[HDR_CURRENT + level];
  if (first === NO_BUCKET) return out;
  const at = (bucket, word) => headerWords + level * ring * metrics * perBucket + (bucket % ring) * metrics * perBucket + slot * perBucket + word;
  const recorded = current - first + 1;
  const covered = Math.min(buckets, recorded, ring);
  const start = current - covered + 1;
  for (let b = start; b <= current; b++) {
    const lo = asValue(words[at(b, 0)], signed), hi = asValue(words[at(b, 1)], signed);
    if (b === start) { out.low = lo; out.high = hi; }
    else { out.low = Math.min(out.low, lo); out.high = Math.max(out.high, hi); }
  }
  out.last = asValue(words[at(current, 2)], signed);
  const before = start > first && (current - (start - 1)) < ring;
  out.delta = before ? out.last - asValue(words[at(start - 1, 2)], signed) : out.last - asValue(words[at(start, 0)], signed);
  out.covered = covered;
  return out;
}

// Every metric's windows: { key: { s1, s10, m1, m10, h1 } }, each a window above.
export function readHistoryWindows(words, bc, schema = METRICS_SCHEMA) {
  const out = {};
  if (!words || !bc || !bc.METRICS_HISTORY_SIZE) return out;
  for (const m of historySlots(schema, bc.METRICS_SIZE >> 2)) {
    const windows = {};
    for (const w of HISTORY_WINDOWS) windows[w.name] = historyWindow(words, bc, w.level, w.buckets, m.slot, m.signed);
    out[m.key] = windows;
  }
  return out;
}
