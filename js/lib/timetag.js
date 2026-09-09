// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//
// timetag.js — OSC time in the browser's terms.
//
// Its own module because it is pure arithmetic with no dependencies, and
// midi_manager.js cannot be imported without the wasm bundle it pulls in. A
// conversion this easy to get subtly wrong should be testable on its own.

// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
export const NTP_EPOCH_OFFSET = 2208988800;

/**
 * An OSC 32.32 NTP timetag → a DOMHighResTimeStamp, for MIDIOutput.send.
 *
 * Two different zeros: an OSC timetag counts from 1900, and performance.now()
 * counts from performance.timeOrigin — which is itself Unix milliseconds. So
 * the conversion runs NTP → Unix → perf, and timeOrigin is what closes it.
 *
 * 0n and 1n are the OSC "immediately" sentinels and become undefined, which is
 * what MIDIOutput.send already reads as now.
 *
 * @param {bigint} when
 * @param {number} [timeOrigin] performance.timeOrigin; injectable for tests.
 * @returns {number|undefined}
 */
export function timetagToPerfMs(when, timeOrigin = performance.timeOrigin) {
  if (when <= 1n) return undefined;
  const secs = Number(when >> 32n) - NTP_EPOCH_OFFSET;
  const frac = Number(when & 0xFFFFFFFFn) / 4294967296;
  return (secs + frac) * 1000 - timeOrigin;
}

/**
 * A DOMHighResTimeStamp (performance.now() terms) → an OSC 32.32 timetag:
 * the moment a device's event ARRIVED, in the form an event carries it. The
 * inverse of timetagToPerfMs, closed by the same timeOrigin.
 *
 * @param {number} perfMs
 * @param {number} [timeOrigin] performance.timeOrigin; injectable for tests.
 * @returns {bigint}
 */
export function perfMsToTimetag(perfMs, timeOrigin = performance.timeOrigin) {
  return unixMsToTimetag(perfMs + timeOrigin);
}

/** Unix milliseconds → an OSC 32.32 timetag. The inverse, for tests and callers. */
export function unixMsToTimetag(ms) {
  const total = ms / 1000 + NTP_EPOCH_OFFSET;
  const secs = Math.floor(total);
  const frac = Math.round((total - secs) * 4294967296);
  return (BigInt(secs) << 32n) | BigInt(frac);
}
