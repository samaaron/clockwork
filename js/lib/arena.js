// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/**
 * arena.js — the arena's table of contents, as JavaScript reads it.
 *
 * Mirrors src/clockwork_arena.h: the header at the front of the arena, its
 * entries, the region ids and what each entry's geometry words mean. Read
 * BY ID, never by position — a reader that walked the table positionally was
 * the drift this replaces (BufferLayout, read as uint32View[n]).
 *
 * `readArena(buffer, base)` returns the header, a `region(id)` lookup, and
 * `constants` — the flat object the rest of the JS has always consumed
 * (IN_BUFFER_START, METRICS_SIZE, SHM_SCOPE_MAX_SCOPES, …), now derived from
 * the table rather than declared beside it.
 */

export const ARENA_MAGIC = 0x43574152;      // 'CWAR'
export const ARENA_VERSION = 1;
export const ARENA_PUBLISHED = 1;
const ENTRY_WORDS = 16;                     // sizeof(ClockworkArenaEntry) / 4
const HEADER_WORDS = 16;                    // the fixed fields before the entries

export const OWNER = Object.freeze({ CLOCKWORK: 1, GUEST: 2, CLIENT: 3, HOST: 4 });

export const REGION = Object.freeze({
  CONTROL: 1, METRICS: 2, NATIVE_STATS: 3, CLOCK_STATE: 4, CLOCK_ANCHORS: 5,
  SAMPLE_CLOCK: 6, CHANNEL_MAP: 7, NODE_ID_COUNTER: 8, IN_RING: 9, OUT_RING: 10,
  NRT_OUT_RING: 11, AUDIO_TAPS: 12, CLIENT_SLOTS: 13, GUEST_CONFIG: 14,
  GUEST_WINDOW: 15, SCOPE: 16, GUEST_PERSIST: 17, TRACK_TAPS: 18,
});

// Geometry word indices, per region (ClockworkArenaGeom).
export const GEOM = Object.freeze({
  RING_MAX_MESSAGE: 0, RING_MESSAGE_MAGIC: 1, RING_PADDING_MAGIC: 2, RING_PADDING_MARKER: 3, RING_HEADER_BYTES: 4,
  METRICS_FIELDS: 0,
  ANCHOR_NTP_START: 0, ANCHOR_DRIFT: 1, ANCHOR_GLOBAL: 2,
  TAPS_SLOTS: 0, TAPS_SLOT_BYTES: 1, TAPS_HEADER_BYTES: 2, TAPS_FRAMES: 3, TAPS_CHANNELS: 4, TAPS_SAMPLE_RATE: 5,
  SCOPE_SLOTS: 0, SCOPE_HEADER_BYTES: 1, SCOPE_SLOT_BYTES: 2, SCOPE_SLOT_HEADER: 3, SCOPE_RING_FRAMES: 4, SCOPE_CHANNELS: 5,
  TRACK_SLOTS: 0, TRACK_SLOT_BYTES: 1, TRACK_SLOT_HEADER: 2, TRACK_RING_FRAMES: 3, TRACK_CHANNELS: 4, TRACK_FIRST_INDEX: 5,
  SLOTS_COUNT: 0, SLOTS_SLOT_BYTES: 1, SLOTS_HEADER_BYTES: 2, SLOTS_STRUCTS_OFF: 3, SLOTS_STRUCTS_BYTES: 4,
  SLOTS_STACK_OFF: 5, SLOTS_STACK_BYTES: 6,
});

/**
 * Read the header at `base` in `buffer`. Throws if it is not a published
 * arena of a version this reader knows, so a caller never proceeds on a
 * table it half-understands.
 *
 * @param {ArrayBuffer|SharedArrayBuffer} buffer
 * @param {number} base byte offset of the arena
 */
export function readArena(buffer, base) {
  if (base % 4 !== 0) throw new Error(`arena base ${base} is not 4-byte aligned`);
  const words = new Uint32Array(buffer, base, HEADER_WORDS);
  const [magic, version, headerBytes, instanceId, arenaBytes, blockBytes, guestOffset, guestBytes,
         entryCount, entryBytes, state] = words;
  if (magic !== ARENA_MAGIC) throw new Error(`not a clockwork arena (magic ${magic.toString(16)})`);
  if (version !== ARENA_VERSION) throw new Error(`arena version ${version}, this reader knows ${ARENA_VERSION}`);
  if (state !== ARENA_PUBLISHED) throw new Error('arena not yet published');
  if (entryBytes !== ENTRY_WORDS * 4) throw new Error(`arena entry shape ${entryBytes} bytes, expected ${ENTRY_WORDS * 4}`);

  const table = new Uint32Array(buffer, base + HEADER_WORDS * 4, entryCount * ENTRY_WORDS);
  const entries = new Map();
  for (let i = 0; i < entryCount; i++) {
    const at = i * ENTRY_WORDS;
    const id = table[at];
    if (id === 0) throw new Error(`arena entry ${i} is empty`);
    const offset = table[at + 1], bytes = table[at + 2];
    if (offset < headerBytes || offset + bytes > arenaBytes) throw new Error(`arena region ${id} lies outside the arena`);
    entries.set(id, { id, offset, bytes, owner: table[at + 3], geom: Array.from(table.subarray(at + 4, at + ENTRY_WORDS)) });
  }
  const region = (id) => {
    const e = entries.get(id);
    if (!e) throw new Error(`arena has no region ${id}`);
    return e;
  };

  return {
    header: { magic, version, headerBytes, instanceId, arenaBytes, blockBytes, guestOffset, guestBytes, entryCount },
    entries,
    region,
    has: (id) => entries.has(id),
    constants: constantsFrom(region, entries, { arenaBytes, blockBytes, guestOffset, guestBytes, version, instanceId }),
  };
}

/**
 * The flat constants object the JS runtime consumes, derived from the table.
 * The names are the ones shared_memory.h has always used, so a reader of
 * either side recognises them; the values come from THIS engine's table.
 */
function constantsFrom(region, entries, h) {
  // Optional: an arena with no track taps has no entry, which is zero slots.
  const track = entries.get(REGION.TRACK_TAPS) || { offset: 0, bytes: 0, geom: [] };
  const inRing = region(REGION.IN_RING), outRing = region(REGION.OUT_RING), nrt = region(REGION.NRT_OUT_RING);
  const control = region(REGION.CONTROL), metrics = region(REGION.METRICS), window = region(REGION.GUEST_WINDOW);
  const anchors = region(REGION.CLOCK_ANCHORS), clock = region(REGION.CLOCK_STATE), taps = region(REGION.AUDIO_TAPS);
  const nodeId = region(REGION.NODE_ID_COUNTER), config = region(REGION.GUEST_CONFIG), scope = region(REGION.SCOPE);
  const sample = region(REGION.SAMPLE_CLOCK), persist = region(REGION.GUEST_PERSIST), map = region(REGION.CHANNEL_MAP);
  const slots = region(REGION.CLIENT_SLOTS);
  return {
    ARENA_VERSION: h.version,
    ARENA_INSTANCE_ID: h.instanceId,
    CLOCKWORK_BLOCK_SIZE: h.blockBytes,
    GUEST_REGION_START: h.guestOffset,
    GUEST_REGION_SIZE: h.guestBytes,
    TOTAL_BUFFER_SIZE: h.arenaBytes,

    IN_BUFFER_START: inRing.offset, IN_BUFFER_SIZE: inRing.bytes,
    OUT_BUFFER_START: outRing.offset, OUT_BUFFER_SIZE: outRing.bytes,
    NRT_OUT_BUFFER_START: nrt.offset, NRT_OUT_BUFFER_SIZE: nrt.bytes,
    MAX_MESSAGE_SIZE: inRing.geom[GEOM.RING_MAX_MESSAGE],
    MESSAGE_MAGIC: inRing.geom[GEOM.RING_MESSAGE_MAGIC],
    PADDING_MAGIC: inRing.geom[GEOM.RING_PADDING_MAGIC],
    RING_PADDING_MARKER: inRing.geom[GEOM.RING_PADDING_MARKER],
    MESSAGE_HEADER_SIZE: inRing.geom[GEOM.RING_HEADER_BYTES],

    CONTROL_START: control.offset, CONTROL_SIZE: control.bytes,
    METRICS_START: metrics.offset, METRICS_SIZE: metrics.geom[GEOM.METRICS_FIELDS] * 4,
    SHM_WINDOW_START: window.offset, SHM_WINDOW_SIZE: window.bytes,
    NTP_START_TIME_START: anchors.offset + anchors.geom[GEOM.ANCHOR_NTP_START], NTP_START_TIME_SIZE: 8,
    DRIFT_OFFSET_START: anchors.offset + anchors.geom[GEOM.ANCHOR_DRIFT], DRIFT_OFFSET_SIZE: 4,
    GLOBAL_OFFSET_START: anchors.offset + anchors.geom[GEOM.ANCHOR_GLOBAL], GLOBAL_OFFSET_SIZE: 4,
    CLOCK_STATE_START: clock.offset, CLOCK_STATE_SIZE: clock.bytes,
    SHM_AUDIO_START: taps.offset, SHM_AUDIO_TOTAL_SIZE: taps.bytes,
    SHM_AUDIO_SLOTS: taps.geom[GEOM.TAPS_SLOTS],
    SHM_AUDIO_OUT_SLOT: 0,   // CLOCKWORK_TAP_OUT: what left for the device
    SHM_AUDIO_IN_SLOT: 1,    // CLOCKWORK_TAP_IN: what arrived from it
    SHM_AUDIO_SLOT_SIZE: taps.geom[GEOM.TAPS_SLOT_BYTES],
    SHM_AUDIO_HEADER_SIZE: taps.geom[GEOM.TAPS_HEADER_BYTES],
    SHM_AUDIO_FRAMES: taps.geom[GEOM.TAPS_FRAMES],
    SHM_AUDIO_CHANNELS: taps.geom[GEOM.TAPS_CHANNELS],
    SHM_AUDIO_SAMPLE_RATE: taps.geom[GEOM.TAPS_SAMPLE_RATE],
    NODE_ID_COUNTER_START: nodeId.offset, NODE_ID_COUNTER_SIZE: nodeId.bytes,
    GUEST_CONFIG_START: config.offset, GUEST_CONFIG_SIZE: config.bytes,
    SHM_SCOPE_START: scope.offset, SHM_SCOPE_TOTAL_SIZE: scope.bytes,
    SHM_SCOPE_MAX_SCOPES: scope.geom[GEOM.SCOPE_SLOTS],
    SHM_SCOPE_HEADER_SIZE: scope.geom[GEOM.SCOPE_HEADER_BYTES],
    SHM_SCOPE_SLOT_SIZE: scope.geom[GEOM.SCOPE_SLOT_BYTES],
    SHM_SCOPE_SLOT_HEADER_SIZE: scope.geom[GEOM.SCOPE_SLOT_HEADER],
    SHM_SCOPE_RING_FRAMES: scope.geom[GEOM.SCOPE_RING_FRAMES],
    SHM_SCOPE_CHANNELS: scope.geom[GEOM.SCOPE_CHANNELS],
    // The engine's track taps: scope slots numbered after the guest's.
    SHM_TRACK_TAPS_START: track.offset,
    SHM_TRACK_TAPS_SIZE: track.bytes,
    SHM_TRACK_TAPS_SLOTS: track.geom[GEOM.TRACK_SLOTS] || 0,
    SHM_TRACK_TAPS_FIRST_INDEX: track.geom[GEOM.TRACK_FIRST_INDEX] || scope.geom[GEOM.SCOPE_SLOTS],
    // Every slot a client can number: the guest's, then the track taps.
    SHM_SCOPE_SLOT_COUNT: scope.geom[GEOM.SCOPE_SLOTS] + (track.geom[GEOM.TRACK_SLOTS] || 0),
    SAMPLE_CLOCK_START: sample.offset, SAMPLE_CLOCK_SIZE: sample.bytes,
    GUEST_PERSIST_START: persist.offset, GUEST_PERSIST_SIZE: persist.bytes,
    CHANNEL_MAP_START: map.offset, CHANNEL_MAP_SIZE: map.bytes,
    CLIENT_SLOTS_START: slots.offset, CLIENT_SLOTS_SIZE: slots.bytes,
    CLIENT_SLOT_COUNT: slots.geom[GEOM.SLOTS_COUNT],
    CLIENT_SLOT_SIZE: slots.geom[GEOM.SLOTS_SLOT_BYTES],
    CLIENT_SLOTS_HEADER_SIZE: slots.geom[GEOM.SLOTS_HEADER_BYTES],
    CLIENT_SLOT_STRUCTS_OFFSET: slots.geom[GEOM.SLOTS_STRUCTS_OFF],
    CLIENT_SLOT_STRUCTS_SIZE: slots.geom[GEOM.SLOTS_STRUCTS_BYTES],
    CLIENT_SLOT_STACK_OFFSET: slots.geom[GEOM.SLOTS_STACK_OFF],
    CLIENT_SLOT_STACK_SIZE: slots.geom[GEOM.SLOTS_STACK_BYTES],
  };
}
