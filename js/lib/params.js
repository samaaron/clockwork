// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * params.js — the parameter table as JavaScript reads and writes it
 * (src/params/clockwork_params.h, docs/PARAMS.md).
 *
 * The region: a header, `capacity` f32 values, `capacity` 96-byte entries, then
 * the text they point into, at the offsets the header gives. readParams() reads
 * a consistent table: the generation before and after, again if it moved.
 * ParamsReader is what a panel or a visualiser holds: refresh() each frame costs
 * one load unless the set of entries moved, and value() reads from the memory.
 * encodeDefine() lays entries out as the define verb's blobs: the same bytes the
 * table holds, little-endian like every target the engine runs on.
 * test/params.test.mjs holds this reader to the fixture the native test writes.
 *
 * What a value means, its place on a knob and its words, is one implementation
 * in Rust on every host: params_manager.js is its door on the web.
 */

export const PARAMS_MAGIC = 0x4357504D;   // 'CWPM'
export const PARAMS_VERSION = 2;
export const HEADER_BYTES = 64;
export const ENTRY_BYTES = 96;
export const NAME_BYTES = 32;

export const KIND = Object.freeze({ RETIRE: 0, CONTINUOUS: 1, STEPPED: 2, SWITCH: 3, NOTE: 4, CHOICE: 5, GROUP: 6 });
export const UNIT = Object.freeze({ NONE: 0, PERCENT: 1, DECIBELS: 2, HERTZ: 3, SECONDS: 4, BPM: 5, SEMITONES: 6, CUSTOM: 7 });
export const FLAG = Object.freeze({ AUTOMATABLE: 1, HIDDEN: 2, READ_ONLY: 4, WRAPS: 8, META: 16 });
export const ROLE = Object.freeze({ NONE: 0, BYPASS: 1, TEMPO: 2, KEY_NOTE: 3, KEY_HELD: 4 });
export const TAPER = Object.freeze({ LINEAR: 0, LOG: -1 });
export const TEXT = Object.freeze({ FULL_NAME: 1, DOC: 2, UNIT: 3, VALUE: 4 });
export const DEFINER_GUEST = 0xFFFFFFFF;
export const DEFINER_HOST = 0xFFFFFFFE;

const KIND_NAMES = Object.freeze(Object.fromEntries(Object.entries(KIND).map(([k, v]) => [v, k.toLowerCase()])));
const UNIT_NAMES = Object.freeze(Object.fromEntries(Object.entries(UNIT).map(([k, v]) => [v, k.toLowerCase()])));

/** The header at `base`, or null when the bytes are not a table this reader knows. */
export function readParamsHeader(buffer, base) {
  const v = new DataView(buffer, base, HEADER_BYTES);
  const magic = v.getUint32(0, true), version = v.getUint32(4, true);
  if (magic !== PARAMS_MAGIC || version !== PARAMS_VERSION) return null;
  const seq = v.getUint32(16, true);
  return {
    capacity: v.getUint32(8, true),
    count: v.getUint32(12, true),
    generation: seq >>> 1,
    writing: (seq & 1) === 1,
    valuesOffset: v.getUint32(20, true),
    entriesOffset: v.getUint32(24, true),
    entryBytes: v.getUint32(28, true),
    textOffset: v.getUint32(32, true),
    textCapacity: v.getUint32(36, true),
    textUsed: v.getUint32(40, true),
  };
}

function seqWord(buffer, base) { return new DataView(buffer, base, HEADER_BYTES).getUint32(16, true); }

/** One slot's value now. */
export function readParamValue(buffer, base, slot, header = readParamsHeader(buffer, base)) {
  if (!header || slot >= header.capacity) return 0;
  return new DataView(buffer, base + header.valuesOffset + slot * 4, 4).getFloat32(0, true);
}

// The run's strings: a tag, UTF-8 and a NUL each. A tag this reader does not know is skipped.
function parseText(run, out) {
  const decoder = new TextDecoder();
  out.fullName = null; out.doc = null; out.unitText = null; out.valueNames = [];
  for (let i = 0; i < run.length;) {
    const tag = run[i++];
    const end = run.indexOf(0, i);
    if (end < 0) return;
    const words = decoder.decode(run.subarray(i, end));
    if (tag === TEXT.FULL_NAME) out.fullName = words;
    else if (tag === TEXT.DOC) out.doc = words;
    else if (tag === TEXT.UNIT) out.unitText = words;
    else if (tag === TEXT.VALUE) out.valueNames.push(words);
    i = end + 1;
  }
}

function readEntry(buffer, at, slot, header, base, bytes) {
  const v = new DataView(buffer, at, ENTRY_BYTES);
  const nameBytes = new Uint8Array(buffer, at + 40, NAME_BYTES);
  let n = nameBytes.indexOf(0); if (n < 0) n = NAME_BYTES;
  const kind = v.getUint32(8, true), unit = v.getUint32(12, true);
  const name = new TextDecoder().decode(nameBytes.slice(0, n));   // a copy: shared memory cannot be decoded in place
  const textAt = v.getUint32(80, true), textBytes = v.getUint32(84, true);
  // An entry read mid-define may say anything: nothing past the bytes is read, and the generation says whether to believe it.
  const inside = textAt + textBytes <= Math.min(header.textCapacity, bytes - header.textOffset);
  const run = inside && textBytes ? new Uint8Array(buffer, base + header.textOffset + textAt, textBytes).slice() : new Uint8Array(0);
  const e = {
    slot,
    id: v.getUint32(4, true),
    name,
    kind, kindName: KIND_NAMES[kind] ?? String(kind),
    unit, unitName: UNIT_NAMES[unit] ?? String(unit),
    flags: v.getUint32(16, true),
    group: v.getUint32(20, true),
    min: v.getFloat32(24, true),
    max: v.getFloat32(28, true),
    def: v.getFloat32(32, true),
    definer: v.getUint32(36, true),
    taper: v.getFloat32(72, true),
    role: v.getUint32(76, true),
    value: readParamValue(buffer, base, slot, header),
    // The entry and its run as they lie, for params_manager.js.
    bytes: new Uint8Array(buffer, at, ENTRY_BYTES).slice(),
    textBytes: run,
  };
  parseText(run, e);
  e.fullName ??= name;
  return e;
}

/**
 * The whole table, consistent: the live entries in slot order, with their
 * values, and the generation they belong to, within `bytes` of `base`. Reads
 * again while a define is writing or if the set moved underneath, up to `tries`
 * times; after that returns what it last read with `torn: true`.
 */
export function readParams(buffer, base, tries = 8, bytes = buffer.byteLength - base) {
  const header = readParamsHeader(buffer, base);
  if (!header) return null;
  // Never past the bytes given: a fixture or a short map holds fewer slots than the capacity.
  const slots = Math.min(header.capacity, Math.floor(Math.max(0, bytes - header.entriesOffset) / header.entryBytes));
  let entries = [], torn = true, before = 0;
  for (let t = 0; t < tries; t++) {
    before = seqWord(buffer, base);
    if (before & 1) continue;
    entries = [];
    for (let s = 0; s < slots; s++) {
      const at = base + header.entriesOffset + s * header.entryBytes;
      if (new DataView(buffer, at, 4).getUint32(0, true) !== 1) continue;
      entries.push(readEntry(buffer, at, s, header, base, bytes));
    }
    if (seqWord(buffer, base) === before) { torn = false; break; }
  }
  return { generation: before >>> 1, capacity: header.capacity, count: header.count, entries, torn };
}

/**
 * What a panel or a visualiser holds. `source` says where the region's bytes are
 * now: a shared buffer that stays, or the latest web snapshot, which is a new
 * buffer each time.
 *
 *   const table = cw.paramsReader();
 *   function frame() {
 *     table.refresh();
 *     for (const e of table.entries) draw(e, table.value(e.slot));
 *   }
 *
 * refresh() costs one load unless the set of entries moved. value() reads the
 * value's word from the memory and allocates nothing. The reader keeps no value
 * of its own, so there is nothing to fall out of step.
 */
export class ParamsReader {
  #source;
  #buffer = null;
  #base = 0;
  #size = 0;
  #valuesWord = 0;
  #capacity = 0;
  #words = null;
  #seen = -1;
  // A word read whole from shared memory, then seen as the float it holds.
  #bits = new Uint32Array(1);
  #float = new Float32Array(this.#bits.buffer);

  /** Every live entry, groups included, in slot order. */
  entries = [];
  /** The generation they belong to. */
  generation = 0;

  /** @param {function(): ({buffer: ArrayBuffer|SharedArrayBuffer, offset: number, size: number}|null)} source */
  constructor(source) { this.#source = source; }

  #locate() {
    const where = this.#source();
    if (!where) return false;
    if (where.buffer === this.#buffer && where.offset === this.#base) return this.#words !== null;
    this.#buffer = where.buffer; this.#base = where.offset; this.#size = where.size; this.#words = null;
    const header = readParamsHeader(where.buffer, where.offset);
    if (!header) return false;
    this.#words = new Uint32Array(where.buffer);
    this.#valuesWord = (where.offset + header.valuesOffset) / 4;
    this.#capacity = header.capacity;
    return true;
  }

  /** Read the facts again if the set of entries moved: true when it did and they were. */
  refresh() {
    if (!this.#locate()) return false;
    const seq = seqWord(this.#buffer, this.#base);
    if (seq === this.#seen) return false;
    const table = readParams(this.#buffer, this.#base, 8, this.#size);
    if (!table || table.torn) return false;
    this.entries = table.entries;
    this.generation = table.generation;
    this.#seen = seq;
    return true;
  }

  /** A slot's value now. 0 before there is a table, or for a slot outside it. */
  value(slot) {
    if (!this.#locate() || slot >= this.#capacity) return 0;
    this.#bits[0] = this.#words[this.#valuesWord + slot];
    return this.#float[0];
  }

  find(id) { return this.entries.find((e) => e.id === id) ?? null; }
  withRole(role) { return role === ROLE.NONE ? null : this.entries.find((e) => e.role === role) ?? null; }
}

function cutToName(bytes) {
  if (bytes.length < NAME_BYTES) return bytes;
  let n = NAME_BYTES - 1;
  while (n > 0 && (bytes[n] & 0xC0) === 0x80) n--;   // on a character's edge
  return bytes.subarray(0, n);
}

/**
 * The define verb's blobs from entries as readParams() returns them (or `{ id }`
 * alone, or `kind: KIND.RETIRE`, to retire), to spread into the send:
 *
 *   cw.send("/clockwork/param/define", ...encodeDefine(entries));
 *
 * The entries, then the text when any has some: `fullName` (when it differs from
 * the name), `doc`, `unitText` and `valueNames`. `live` and `definer` are the
 * engine's and are written as 0.
 */
export function encodeDefine(entries) {
  const out = new Uint8Array(entries.length * ENTRY_BYTES);
  const v = new DataView(out.buffer);
  const enc = new TextEncoder();
  const text = [];
  let textBytes = 0;
  const say = (tag, words) => {
    const encoded = enc.encode(words);
    text.push(Uint8Array.of(tag), encoded, Uint8Array.of(0));
    textBytes += encoded.length + 2;
  };
  entries.forEach((e, i) => {
    const at = i * ENTRY_BYTES;
    v.setUint32(at + 4, e.id >>> 0, true);
    v.setUint32(at + 8, (e.kind ?? KIND.RETIRE) >>> 0, true);
    v.setUint32(at + 12, (e.unit ?? UNIT.NONE) >>> 0, true);
    v.setUint32(at + 16, (e.flags ?? 0) >>> 0, true);
    v.setUint32(at + 20, (e.group ?? 0) >>> 0, true);
    v.setFloat32(at + 24, e.min ?? 0, true);
    v.setFloat32(at + 28, e.max ?? 0, true);
    v.setFloat32(at + 32, e.def ?? 0, true);
    out.set(cutToName(enc.encode(e.name ?? "")), at + 40);
    v.setFloat32(at + 72, e.taper ?? TAPER.LINEAR, true);
    v.setUint32(at + 76, (e.role ?? ROLE.NONE) >>> 0, true);
    const start = textBytes;
    if (e.fullName && e.fullName !== e.name) say(TEXT.FULL_NAME, e.fullName);
    if (e.doc) say(TEXT.DOC, e.doc);
    if (e.unitText) say(TEXT.UNIT, e.unitText);
    for (const name of e.valueNames ?? []) say(TEXT.VALUE, name);
    if (textBytes !== start) {
      v.setUint32(at + 80, start, true);
      v.setUint32(at + 84, textBytes - start, true);
    }
  });
  if (textBytes === 0) return [out];
  const blob = new Uint8Array(textBytes);
  let at = 0;
  for (const part of text) { blob.set(part, at); at += part.length; }
  return [out, blob];
}
