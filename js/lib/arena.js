// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/**
 * arena.js — the arena's table of contents, as JavaScript reads it.
 *
 * Mirrors src/clockwork_arena.h: the header at the front of the arena, its
 * table of areas, its table of regions, the ids, and what each entry's
 * geometry words mean. Read BY ID, never by position — a reader that walked
 * the table positionally was the drift this replaces (BufferLayout, read as
 * uint32View[n]).
 *
 * `readArena(buffer, base)` returns the header, the areas, a `region(id)`
 * lookup, and `constants` — the flat object the rest of the JS has always
 * consumed (IN_RING_START, METRICS_SIZE, TAPS_SLOTS, …), now derived from
 * the table rather than declared beside it.
 */

export const ARENA_MAGIC = 0x43574152;      // 'CWAR'
export const ARENA_VERSION = 2;
export const ARENA_PUBLISHED = 1;
const HEADER_WORDS = 16;                    // the fixed fields before the areas
const MAX_AREAS = 16;
const AREA_WORDS = 6;                       // sizeof(ClockworkArenaAreaEntry) / 4
const ENTRY_WORDS = 16;                     // sizeof(ClockworkArenaEntry) / 4
const GEOM_WORDS = 8;
const ENTRIES_AT = HEADER_WORDS + MAX_AREAS * AREA_WORDS;   // in words from the base

// The fifteen areas (ClockworkArenaArea): five in the arena, six outside it, the program's own three, and the
// engine's memory, the one block the arena and the outside ones are carved from.
export const AREA = Object.freeze({
  CLOCKWORK_STATE: 1, CLOCKWORK_AUDIO: 2, CLOCKWORK_TRANSPORT: 3, GUEST_PUBLIC: 4, GUEST_PRIVATE: 5,
  ASSETS: 6, RETURNS: 7, GUEST_MEMORY_BULK: 8, GUEST_MEMORY_FAST: 9,
  STATIC_DATA: 10, STACK: 11, MALLOC: 12, RUST_HEAP: 13,
  ENGINE_MEMORY: 14,
});
// Where an area's base is measured from (ClockworkArenaSpace).
export const SPACE = Object.freeze({ UNKNOWN: 0, ARENA: 1, SEGMENT: 2, PROCESS: 3 });

export const OWNER = Object.freeze({ CLOCKWORK: 1, GUEST: 2, CLIENT: 3, HOST: 4 });
// Who READS a region: a bit set. Owner says who writes; this says who it is for.
export const AUDIENCE = Object.freeze({ PUBLIC: 1, GUEST: 2, HOST: 4, TRANSPORT: 8 });
// How often the engine thread touches a region.
export const CADENCE = Object.freeze({ BLOCK: 1, CHANGE: 2, BOOT: 3 });
// The RAM speed a region needs.
export const TIER = Object.freeze({ FAST: 1, BULK: 2 });
// When a region's bytes are reset.
export const LIFETIME = Object.freeze({ ARENA: 1, COLD_BOOT: 2, REBUILD: 3 });

// In the order the regions lie in memory (ClockworkArenaRegion, version 2), each named for its half.
export const REGION = Object.freeze({
  CLOCKWORK_METRICS: 1, CLOCKWORK_NATIVE_STATS: 2, CLOCKWORK_CLOCK_ANCHORS: 3, CLOCKWORK_CLOCK_STATE: 4,
  CLOCKWORK_SAMPLE_CLOCK: 5, CLOCKWORK_CHANNEL_MAP: 6,
  CLOCKWORK_SCOPE: 7, CLOCKWORK_TAPS: 8,
  CLOCKWORK_RING_CURSORS: 9, CLOCKWORK_NODE_ID_COUNTER: 10, CLOCKWORK_IN_RING: 11, CLOCKWORK_RT_OUT_RING: 12,
  CLOCKWORK_NRT_OUT_RING: 13,
  CLOCKWORK_CONTROL_RING: 17,   // clockwork's own control verbs (docs/RESTARTS.md); a later addition, numbered after
  GUEST_WINDOW: 14,
  GUEST_CONFIG: 15, GUEST_PERSIST: 16,
});

// Geometry word indices, per region (ClockworkArenaGeom).
export const GEOM = Object.freeze({
  RING_MAX_MESSAGE: 0, RING_MESSAGE_MAGIC: 1, RING_PADDING_MAGIC: 2, RING_PADDING_MARKER: 3, RING_HEADER_BYTES: 4,
  METRICS_FIELDS: 0,
  ANCHOR_NTP_START: 0, ANCHOR_DRIFT: 1, ANCHOR_GLOBAL: 2,
  SCOPE_SLOTS: 0, SCOPE_SLOT_BYTES: 1, SCOPE_HEADER_BYTES: 2, SCOPE_FRAMES: 3, SCOPE_CHANNELS: 4, SCOPE_SAMPLE_RATE: 5,
  // taps: a header, the guest's TAP_SLOTS slots, then TAP_TRACK_SLOTS track slots, one index space
  TAP_SLOTS: 0, TAP_HEADER_BYTES: 1, TAP_SLOT_BYTES: 2, TAP_SLOT_HEADER: 3, TAP_RING_FRAMES: 4, TAP_CHANNELS: 5,
  TAP_TRACK_SLOTS: 6, TAP_TRACK_FIRST: 7,
  // the guest window: what the guest declared its window to be, 0 until it binds
  WINDOW_MAGIC: 0, WINDOW_VERSION: 1,
});

/** The names a map shows, by id: `clockwork-metrics`, `guest-window`, … */
export const REGION_NAMES = Object.freeze(Object.fromEntries(Object.entries(REGION).map(([k, v]) => [v, k.toLowerCase().replace(/_/g, '-')])));
export const AREA_NAMES = Object.freeze(Object.fromEntries(Object.entries(AREA).map(([k, v]) => [v, k.toLowerCase().replace(/_/g, '-')])));

const PUBLIC_AREAS = new Set([AREA.CLOCKWORK_STATE, AREA.CLOCKWORK_AUDIO, AREA.GUEST_PUBLIC]);

/**
 * A region's bytes, as a view over `buffer`: what the C ABI's clockwork_client_region hands a
 * client, base and length, for a reader that wants the memory itself (a memory viewer, a dump).
 * @param {ArrayBufferLike} buffer
 * @param {number} base  the arena's base in the buffer
 * @param {{ offset: number, bytes: number }} entry  from readArena's entries
 */
export function regionBytes(buffer, base, entry) {
  const at = base + entry.offset;
  if (at + entry.bytes > buffer.byteLength) throw new Error(`region at ${at} runs past the buffer (${entry.bytes} bytes)`);
  return new Uint8Array(buffer, at, entry.bytes);
}

/**
 * Read the header at `base` in `buffer`. Throws if it is not a published
 * arena of a version this reader knows, or if its tables disagree with
 * themselves, so a caller never proceeds on a table it half-understands.
 * The checks are clockwork_arena_check's, in its words.
 *
 * @param {ArrayBuffer|SharedArrayBuffer} buffer
 * @param {number} base byte offset of the arena
 */
export function readArena(buffer, base) {
  if (base % 4 !== 0) throw new Error(`arena base ${base} is not 4-byte aligned`);
  const words = new Uint32Array(buffer, base, HEADER_WORDS);
  const [magic, version, headerBytes, instanceId, arenaBytes, clockworkBytes, guestOffset, guestBytes,
         areaCount, areaBytes, entryCount, entryBytes, state, generation] = words;
  if (magic !== ARENA_MAGIC) throw new Error(`not a clockwork arena (magic ${magic.toString(16)})`);
  if (version !== ARENA_VERSION) throw new Error(`arena version ${version}, this reader knows ${ARENA_VERSION}`);
  if (state !== ARENA_PUBLISHED) throw new Error('arena not yet published');
  if (areaBytes !== AREA_WORDS * 4) throw new Error(`arena area shape ${areaBytes} bytes, expected ${AREA_WORDS * 4}`);
  if (entryBytes !== ENTRY_WORDS * 4) throw new Error(`arena entry shape ${entryBytes} bytes, expected ${ENTRY_WORDS * 4}`);
  if (areaCount > MAX_AREAS) throw new Error('more areas than the table holds');
  if (clockworkBytes > arenaBytes) throw new Error("clockwork's half larger than the arena");
  if (guestOffset !== clockworkBytes) throw new Error("the guest's half does not start where clockwork's ends");
  if (guestBytes !== arenaBytes - guestOffset) throw new Error('the two halves do not cover the arena');

  // The areas. In-arena ones carry offsets; those outside carry an address in a space.
  const areaTable = new Uint32Array(buffer, base + HEADER_WORDS * 4, areaCount * AREA_WORDS);
  const areas = new Map();
  for (let i = 0; i < areaCount; i++) {
    const at = i * AREA_WORDS;
    const area = areaTable[at], space = areaTable[at + 1];
    if (area === 0) throw new Error('an empty area inside the count');
    if (areas.has(area)) throw new Error('an area listed twice');
    const addr = areaTable[at + 2] + areaTable[at + 3] * 2 ** 32;
    const bytes = areaTable[at + 4] + areaTable[at + 5] * 2 ** 32;
    if (space === SPACE.ARENA) {
      const guests = area === AREA.GUEST_PUBLIC || area === AREA.GUEST_PRIVATE;
      if (addr < headerBytes || addr + bytes > arenaBytes) throw new Error('an area outside the arena');
      if (guests ? addr < guestOffset : addr + bytes > clockworkBytes) throw new Error('an area in the wrong half');
      for (const b of areas.values())
        if (b.space === SPACE.ARENA && addr < b.base + b.bytes && b.base < addr + bytes) throw new Error('two areas overlap');
    } else if (area <= AREA.GUEST_PRIVATE) {
      throw new Error('an area of the arena placed outside it');
    }
    areas.set(area, { area, space, base: addr, bytes });
  }
  const areaOf = (e) => {
    for (const a of areas.values())
      if (a.space === SPACE.ARENA && e.offset >= a.base && e.offset + e.bytes <= a.base + a.bytes) return a.area;
    return 0;
  };

  // The regions, with the five facts of each.
  const table = new Uint32Array(buffer, base + ENTRIES_AT * 4, entryCount * ENTRY_WORDS);
  const entries = new Map();
  for (let i = 0; i < entryCount; i++) {
    const at = i * ENTRY_WORDS;
    const id = table[at];
    if (id === 0) throw new Error(`arena entry ${i} is empty`);
    const offset = table[at + 1], bytes = table[at + 2];
    if (offset < headerBytes || offset + bytes > arenaBytes) throw new Error(`arena region ${id} lies outside the arena`);
    const [owner, audience, cadence, tier, lifetime] = table.subarray(at + 3, at + 8);
    if (owner < 1 || owner > 4) throw new Error(`arena region ${id} has no owner`);
    if (audience < 1 || audience > 15) throw new Error(`arena region ${id} has no audience`);
    if (cadence < 1 || cadence > 3) throw new Error(`arena region ${id} has no cadence`);
    if (tier < 1 || tier > 2) throw new Error(`arena region ${id} has no tier`);
    if (lifetime < 1 || lifetime > 3) throw new Error(`arena region ${id} has no lifetime`);
    const geom = Array.from(table.subarray(at + 8, at + 8 + GEOM_WORDS));
    const e = { id, offset, bytes, owner, audience, cadence, tier, lifetime, geom };
    const area = areaOf(e);
    if (area === 0) throw new Error(`arena region ${id} is in no area`);
    const pub = (audience & AUDIENCE.PUBLIC) !== 0;
    if (PUBLIC_AREAS.has(area) && !pub) throw new Error(`arena region ${id} is in a public area but not public`);
    if (!PUBLIC_AREAS.has(area) && pub) throw new Error(`arena region ${id} is public but outside the public areas`);
    if (area === AREA.CLOCKWORK_TRANSPORT && (audience & AUDIENCE.TRANSPORT) === 0) throw new Error(`arena region ${id} is in the transport area but not transport`);
    e.area = area;
    entries.set(id, e);
  }
  const region = (id) => {
    const e = entries.get(id);
    if (!e) throw new Error(`arena has no region ${id}`);
    return e;
  };
  // A data contract a client may read by hand: the region exists and says so.
  const isPublic = (id) => {
    const e = entries.get(id);
    return !!e && (e.audience & AUDIENCE.PUBLIC) !== 0;
  };

  return {
    // `generation` is the word as it was at this read; readGeneration below
    // reads it live, since it moves while a page holds the table.
    header: { magic, version, headerBytes, instanceId, arenaBytes, clockworkBytes, guestOffset, guestBytes,
              areaCount, entryCount, generation },
    areas,
    area: (id) => areas.get(id) ?? null,
    entries,
    region,
    areaOf,
    public: isPublic,
    has: (id) => entries.has(id),
    // The worklet's flat constants need every region the engine carves; a
    // reader of some other arena (a test's, a partial one) may never ask.
    get constants() {
      return constantsFrom(region, { arenaBytes, clockworkBytes, guestOffset, guestBytes, version, instanceId });
    },
  };
}

/**
 * The generation word (ClockworkArenaHeader::generation, docs/RESTARTS.md):
 * odd while there is no guest instance, even while there is one; the rest is
 * the generation the last build opened — the number /clockwork/setup named.
 * The engine moves it at every teardown and build, so a page reads it live,
 * with one load, rather than from a readArena snapshot.
 */
const GENERATION_WORD = 13;
/** The word itself, to read again and again without making a view each time (a door asks it on every send). */
export function generationView(buffer, base) {
  return new Uint32Array(buffer, base + GENERATION_WORD * 4, 1);
}
export function readGeneration(buffer, base) {
  return Atomics.load(generationView(buffer, base), 0);
}
/** The generation the word names: the last one opened, 0 before any. */
export function instanceGeneration(word) { return word >>> 1; }
/** Whether a guest instance is up, from the word. */
export function hasInstance(word) { return (word & 1) === 0; }

/**
 * The flat constants object the JS runtime consumes, derived from the table.
 * The names are the ones shared_memory.h has always used, so a reader of
 * either side recognises them; the values come from THIS engine's table.
 */
function constantsFrom(region, h) {
  const inRing = region(REGION.CLOCKWORK_IN_RING), outRing = region(REGION.CLOCKWORK_RT_OUT_RING), nrt = region(REGION.CLOCKWORK_NRT_OUT_RING);
  const cursors = region(REGION.CLOCKWORK_RING_CURSORS), metrics = region(REGION.CLOCKWORK_METRICS), window = region(REGION.GUEST_WINDOW);
  const anchors = region(REGION.CLOCKWORK_CLOCK_ANCHORS), clock = region(REGION.CLOCKWORK_CLOCK_STATE), scope = region(REGION.CLOCKWORK_SCOPE);
  const nodeId = region(REGION.CLOCKWORK_NODE_ID_COUNTER), config = region(REGION.GUEST_CONFIG), taps = region(REGION.CLOCKWORK_TAPS);
  const sample = region(REGION.CLOCKWORK_SAMPLE_CLOCK), persist = region(REGION.GUEST_PERSIST), map = region(REGION.CLOCKWORK_CHANNEL_MAP);
  let controlRing = null;
  try { controlRing = region(REGION.CLOCKWORK_CONTROL_RING); } catch { /* an arena without one */ }
  return {
    ARENA_VERSION: h.version,
    ARENA_INSTANCE_ID: h.instanceId,
    CLOCKWORK_HALF_SIZE: h.clockworkBytes,
    GUEST_HALF_START: h.guestOffset,
    GUEST_HALF_SIZE: h.guestBytes,
    ARENA_SIZE: h.arenaBytes,

    IN_RING_START: inRing.offset, IN_RING_SIZE: inRing.bytes,
    RT_OUT_RING_START: outRing.offset, RT_OUT_RING_SIZE: outRing.bytes,
    NRT_OUT_RING_START: nrt.offset, NRT_OUT_RING_SIZE: nrt.bytes,
    // Clockwork's own control verbs (docs/RESTARTS.md). The web writes
    // nothing there — the worklet has no control pass, so the serve word is
    // never raised and every verb rides the in ring — but a reader of the
    // table sees the region as the engine lays it out. 0 in an arena
    // without one (a test's table).
    CONTROL_RING_START: controlRing ? controlRing.offset : 0, CONTROL_RING_SIZE: controlRing ? controlRing.bytes : 0,
    MAX_MESSAGE_SIZE: inRing.geom[GEOM.RING_MAX_MESSAGE],
    MESSAGE_MAGIC: inRing.geom[GEOM.RING_MESSAGE_MAGIC],
    PADDING_MAGIC: inRing.geom[GEOM.RING_PADDING_MAGIC],
    RING_PADDING_MARKER: inRing.geom[GEOM.RING_PADDING_MARKER],
    MESSAGE_HEADER_SIZE: inRing.geom[GEOM.RING_HEADER_BYTES],

    RING_CURSORS_START: cursors.offset, RING_CURSORS_SIZE: cursors.bytes,
    METRICS_START: metrics.offset, METRICS_SIZE: metrics.geom[GEOM.METRICS_FIELDS] * 4,
    GUEST_WINDOW_START: window.offset, GUEST_WINDOW_SIZE: window.bytes,
    NTP_START_TIME_START: anchors.offset + anchors.geom[GEOM.ANCHOR_NTP_START], NTP_START_TIME_SIZE: 8,
    DRIFT_OFFSET_START: anchors.offset + anchors.geom[GEOM.ANCHOR_DRIFT], DRIFT_OFFSET_SIZE: 4,
    GLOBAL_OFFSET_START: anchors.offset + anchors.geom[GEOM.ANCHOR_GLOBAL], GLOBAL_OFFSET_SIZE: 4,
    CLOCK_STATE_START: clock.offset, CLOCK_STATE_SIZE: clock.bytes,
    SCOPE_START: scope.offset, SCOPE_SIZE: scope.bytes,
    SCOPE_SLOTS: scope.geom[GEOM.SCOPE_SLOTS],
    SCOPE_OUT_SLOT: 0,   // CLOCKWORK_TAP_OUT: what left for the device
    SCOPE_IN_SLOT: 1,    // CLOCKWORK_TAP_IN: what arrived from it
    SCOPE_SLOT_SIZE: scope.geom[GEOM.SCOPE_SLOT_BYTES],
    SCOPE_HEADER_SIZE: scope.geom[GEOM.SCOPE_HEADER_BYTES],
    SCOPE_FRAMES: scope.geom[GEOM.SCOPE_FRAMES],
    SCOPE_CHANNELS: scope.geom[GEOM.SCOPE_CHANNELS],
    SCOPE_SAMPLE_RATE: scope.geom[GEOM.SCOPE_SAMPLE_RATE],
    NODE_ID_COUNTER_START: nodeId.offset, NODE_ID_COUNTER_SIZE: nodeId.bytes,
    GUEST_CONFIG_START: config.offset, GUEST_CONFIG_SIZE: config.bytes,
    // One tap region: the guest's slots, then the engine's track slots, one index space.
    TAPS_START: taps.offset, TAPS_SIZE: taps.bytes,
    TAPS_SLOTS: taps.geom[GEOM.TAP_SLOTS],
    TAPS_HEADER_SIZE: taps.geom[GEOM.TAP_HEADER_BYTES],
    TAP_SLOT_SIZE: taps.geom[GEOM.TAP_SLOT_BYTES],
    TAP_SLOT_HEADER_SIZE: taps.geom[GEOM.TAP_SLOT_HEADER],
    TAP_RING_FRAMES: taps.geom[GEOM.TAP_RING_FRAMES],
    TAP_CHANNELS: taps.geom[GEOM.TAP_CHANNELS],
    TAP_TRACK_SLOTS: taps.geom[GEOM.TAP_TRACK_SLOTS],
    TAP_TRACK_FIRST: taps.geom[GEOM.TAP_TRACK_FIRST],
    TAP_SLOT_COUNT: taps.geom[GEOM.TAP_SLOTS] + taps.geom[GEOM.TAP_TRACK_SLOTS],
    SAMPLE_CLOCK_START: sample.offset, SAMPLE_CLOCK_SIZE: sample.bytes,
    GUEST_PERSIST_START: persist.offset, GUEST_PERSIST_SIZE: persist.bytes,
    CHANNEL_MAP_START: map.offset, CHANNEL_MAP_SIZE: map.bytes,
  };
}
