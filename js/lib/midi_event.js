// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/**
 * Reading a `/clockwork/midi/in/*` event.
 *
 * An event is the one form MIDI takes once it is inside clockwork, whether a native connection produced it or
 * a web host put it on the ingress. So this decodes the EVENT, not a device's bytes: a consumer written
 * against it reads the same thing on either host, and there is no second path from the device that could
 * bypass the engine.
 *
 * It is plain JS on purpose. What must never drift between the two hosts is the WIRE FORMAT — `kind()` and
 * `encode_in` in the Rust core are the one source of truth for that. Reading a packet clockwork itself wrote
 * needs no wasm, and a consumer should not have to start a second instance of a 96 kB module to learn that a
 * note was played.
 */
const IN_PREFIX = "/clockwork/midi/in/";

// The type tag string, which is what tells a timetag from an int: both decode to a number.
function typeTags(bytes) {
  let end = 0;
  while (end < bytes.length && bytes[end] !== 0) end++;
  let p = (end + 4) & ~3;
  if (bytes[p] !== 0x2c) return "";      // ','
  let e = p;
  while (e < bytes.length && bytes[e] !== 0) e++;
  return String.fromCharCode.apply(null, bytes.subarray(p + 1, e));
}

/**
 * A `/clockwork/midi/in/*` packet → `[kind, port, ...values]`, e.g. `["note_on", "kbd", 1, 60, 100]`.
 *
 * The values are the event's own, in order, and `kind` says what shape they take: a channel message carries
 * its channel first, a transport message carries nothing. The arrival timetag a host appends is dropped — it
 * is when the event reached the host, not something the message said.
 *
 * @param {Uint8Array} osc the packet
 * @param {(data: Uint8Array) => any[]} decodePacket an OSC decoder, e.g. osc_fast's
 * @returns {any[]|null} the fields, or null when this is not an inbound MIDI event
 */
export function midiInDecode(osc, decodePacket) {
  if (!osc || osc[0] !== 0x2f) return null;   // '/': a message, not a bundle
  let msg;
  try { msg = decodePacket(osc); } catch { return null; }
  const address = msg[0];
  if (typeof address !== "string" || !address.startsWith(IN_PREFIX)) return null;
  const kind = address.slice(IN_PREFIX.length);
  if (!kind || kind.includes("/")) return null;
  const tags = typeTags(osc);
  const out = [kind];
  for (let i = 1; i < msg.length; i++) if (tags[i - 1] !== "t") out.push(msg[i]);
  return out;
}
