// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The web reader (js/lib/params.js) against the table the native engine
// wrote: test/fixtures/params_region.bin is what test_params.cpp's fixture
// case produced, and both readers must read it the same.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { ParamsReader, readParams, readParamsHeader, readParamValue, encodeDefine, KIND, UNIT, FLAG, ROLE, TAPER, TEXT, ENTRY_BYTES, HEADER_BYTES } from "../js/lib/params.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const fixture = () => {
  const bytes = readFileSync(join(root, "test/fixtures/params_region.bin"));
  const buffer = new ArrayBuffer(bytes.length);
  new Uint8Array(buffer).set(bytes);
  return buffer;
};

test("the header is the engine's", () => {
  const h = readParamsHeader(fixture(), 0);
  assert.ok(h);
  assert.equal(h.generation, 1);
  assert.equal(h.writing, false);
  assert.equal(h.count, 5);
  assert.equal(h.entryBytes, ENTRY_BYTES);
  assert.equal(h.valuesOffset, HEADER_BYTES);
  assert.equal(h.entriesOffset % 16, 0);
  assert.equal(h.textOffset % 16, 0);
  assert.ok(h.capacity >= 5);
  assert.ok(h.textUsed > 0 && h.textUsed <= h.textCapacity);
});

test("the entries read as the native test defined them, with their text and values", () => {
  const t = readParams(fixture(), 0);
  assert.equal(t.torn, false);
  assert.equal(t.generation, 1);
  assert.equal(t.entries.length, 5);
  const [tone, cutoff, steps, on, wave] = t.entries;
  assert.deepEqual([tone.id, tone.name, tone.kind, tone.kindName, tone.doc], [9, "tone", KIND.GROUP, "group", "What shapes the tone"]);
  assert.deepEqual(
    [cutoff.id, cutoff.name, cutoff.fullName, cutoff.doc, cutoff.kind, cutoff.kindName, cutoff.min, cutoff.max, cutoff.def, cutoff.unit, cutoff.unitName, cutoff.flags, cutoff.group, cutoff.taper, cutoff.value, cutoff.definer],
    [1, "cutoff", "Filter cutoff", "Where the filter closes", KIND.CONTINUOUS, "continuous", 20, 20000, 1000, UNIT.HERTZ, "hertz", FLAG.AUTOMATABLE, 9, TAPER.LOG, 440, 7]);
  assert.equal(readParamValue(fixture(), 0, cutoff.slot), 440);
  assert.deepEqual([steps.id, steps.name, steps.fullName, steps.kind, steps.max, steps.def, steps.flags, steps.unitText, steps.value],
    [2, "steps", "steps", KIND.STEPPED, 7, 3, FLAG.HIDDEN, "steps", 3]);
  assert.deepEqual([on.id, on.name, on.kind, on.flags, on.group, on.role, on.value], [3, "on", KIND.SWITCH, 0, 0, ROLE.BYPASS, 1]);
  assert.deepEqual([wave.id, wave.kind, wave.valueNames, wave.value], [4, KIND.CHOICE, ["saw", "square", "é"], 1]);
});

test("encodeDefine lays entries out as the table holds them", () => {
  const buffer = fixture();
  const h = readParamsHeader(buffer, 0);
  const t = readParams(buffer, 0);
  const [blob, text] = encodeDefine(t.entries);
  const table = new Uint8Array(buffer, h.entriesOffset, t.entries.length * ENTRY_BYTES);
  let textAt = 0;
  t.entries.forEach((e, i) => {
    const mine = blob.subarray(i * ENTRY_BYTES, (i + 1) * ENTRY_BYTES);
    const theirs = Uint8Array.from(table.subarray(i * ENTRY_BYTES, (i + 1) * ENTRY_BYTES));
    // live and definer are the engine's: zero in a define. A run's place is the blob's there and the region's here.
    theirs.fill(0, 0, 4); theirs.fill(0, 36, 40);
    new DataView(theirs.buffer).setUint32(80, e.textBytes.length ? textAt : 0, true);
    assert.deepEqual(mine, theirs, `entry ${i}`);
    // The same run, in the same order.
    assert.deepEqual(text.subarray(textAt, textAt + e.textBytes.length), e.textBytes, `text of entry ${i}`);
    textAt += e.textBytes.length;
  });
  assert.equal(textAt, text.length);
  // With no text, one blob. A retire is an id and nothing else.
  const retire = encodeDefine([{ id: 9 }]);
  assert.equal(retire.length, 1);
  assert.equal(retire[0].length, ENTRY_BYTES);
  assert.equal(new DataView(retire[0].buffer).getUint32(4, true), 9);
  assert.equal(new DataView(retire[0].buffer).getUint32(8, true), KIND.RETIRE);
});

test("encodeDefine cuts a name on a character's edge", () => {
  const [blob] = encodeDefine([{ id: 1, name: "é".repeat(40), kind: KIND.CONTINUOUS, max: 1 }]);
  const name = blob.subarray(40, 72);
  const n = name.indexOf(0);
  assert.equal(new TextDecoder("utf-8", { fatal: true }).decode(name.subarray(0, n)), "é".repeat(15));
});

test("a table mid-write reads again, and says when it could not", () => {
  const buffer = fixture();
  new DataView(buffer).setUint32(16, 3, true);   // odd: a define is writing
  const t = readParams(buffer, 0, 3);
  assert.equal(t.torn, true);
  assert.equal(t.generation, 1);
});

test("text that points outside the region reads as none", () => {
  const buffer = fixture();
  const h = readParamsHeader(buffer, 0);
  new DataView(buffer).setUint32(h.entriesOffset + h.entryBytes + 80, 0xFFFFFFF0, true);   // cutoff's run, sent far away
  const cutoff = readParams(buffer, 0).entries[1];
  assert.equal(cutoff.doc, null);
  assert.equal(cutoff.fullName, "cutoff");
});

test("a reader keeps the facts until the generation moves, and reads each value from the memory", () => {
  const buffer = fixture();
  const h = readParamsHeader(buffer, 0);
  const v = new DataView(buffer);
  const r = new ParamsReader(() => ({ buffer, offset: 0, size: buffer.byteLength }));
  assert.equal(r.refresh(), true);
  assert.equal(r.generation, 1);
  const before = r.entries;
  assert.equal(r.refresh(), false);
  assert.equal(r.entries, before, "nothing moved, nothing read");

  const cutoff = r.find(1);
  assert.equal(r.value(cutoff.slot), 440);
  v.setFloat32(h.valuesOffset + cutoff.slot * 4, 880, true);
  assert.equal(r.value(cutoff.slot), 880, "no refresh, and no copy");
  assert.equal(r.refresh(), false, "a value moving is not the set of entries moving");
  assert.equal(r.value(100000), 0);
  assert.equal(r.withRole(ROLE.BYPASS).id, 3);
  assert.equal(r.withRole(ROLE.NONE), null);

  // A define under way: what was read stands. Landed: read again.
  v.setUint32(16, 3, true);
  assert.equal(r.refresh(), false);
  assert.equal(r.generation, 1);
  v.setUint32(16, 4, true);
  assert.equal(r.refresh(), true);
  assert.equal(r.generation, 2);
});

test("a reader follows the table from one snapshot's buffer to the next", () => {
  // postMessage mode: each snapshot is a new buffer, at an offset past the metrics.
  const at = 1024;
  const wrap = (mutate) => {
    const inner = fixture();
    mutate?.(new DataView(inner));
    const outer = new ArrayBuffer(at + inner.byteLength);
    new Uint8Array(outer).set(new Uint8Array(inner), at);
    return outer;
  };
  const h = readParamsHeader(fixture(), 0);
  let snapshot = null;
  const r = new ParamsReader(() => (snapshot ? { buffer: snapshot, offset: at, size: snapshot.byteLength - at } : null));
  assert.equal(r.refresh(), false, "no snapshot yet");
  assert.equal(r.value(0), 0);
  snapshot = wrap();
  assert.equal(r.refresh(), true);
  const before = r.entries;
  const slot = r.find(1).slot;
  assert.equal(r.value(slot), 440);
  snapshot = wrap((v) => v.setFloat32(h.valuesOffset + slot * 4, 220, true));
  assert.equal(r.refresh(), false, "a new buffer, the same facts");
  assert.equal(r.entries, before);
  assert.equal(r.value(slot), 220);
  assert.ok(TEXT.DOC);
});
