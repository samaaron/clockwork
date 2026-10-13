// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The ring corpus, replayed through js/lib/ring.js.
//
// test/fixtures/ring_wire.txt is a set of writes and drains with the bytes
// they must leave behind. test_ring_wire_conformance.cpp replays it through
// the C++ writer and reader; this replays it through the JavaScript, the same
// way, and requires the same deliveries, cursors and ring image byte for byte.
// The two implementations cannot disagree about the format without one of
// them failing here or there.
import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { ringAt, ringWrite, ringDrain } from "../js/lib/ring.js";

const here = path.dirname(fileURLToPath(import.meta.url));
const corpusPath = path.join(here, "fixtures", "ring_wire.txt");

const toHex = (bytes) => Buffer.from(bytes).toString("hex");
const fromHex = (hex) => new Uint8Array(Buffer.from(hex, "hex"));

// The same reading as the C++ test's loadCorpus: one keyword per line.
function loadCorpus() {
  const cases = [];
  let cur = null;
  for (const raw of fs.readFileSync(corpusPath, "utf8").split("\n")) {
    const line = raw.trim();
    if (!line || line.startsWith("#")) continue;
    const [kw, ...f] = line.split(/\s+/);
    switch (kw) {
      case "case": cur = { name: f[0], size: 0, ops: [], msgs: [], head: 0, tail: 0, image: "" }; break;
      case "size": cur.size = Number(f[0]); break;
      case "write": cur.ops.push({ write: true, sourceId: Number(f[0]), payload: fromHex(f[1]), expect: f[2] }); break;
      case "drain": cur.ops.push({ write: false, max: Number(f[0]) }); break;
      case "msg": cur.msgs.push({ seq: Number(f[0]), sourceId: Number(f[1]), payload: f[2] ?? "" }); break;
      case "head": cur.head = Number(f[0]); break;
      case "tail": cur.tail = Number(f[0]); break;
      case "image": cur.image = f[0]; break;
      case "end": cases.push(cur); break;
      default: throw new Error(`ring_wire.txt: unknown line "${line}"`);
    }
  }
  return cases;
}

test("ring wire conformance (JavaScript): the corpus replays byte for byte", () => {
  const cases = loadCorpus();
  assert.ok(cases.length >= 5, "the corpus is there and was read");

  for (const c of cases) {
    // As the C++ test: a zeroed ring, every cursor at 0, a lock word that
    // starts free.
    const words = new Int32Array(4);
    const ring = ringAt(new ArrayBuffer(c.size), 0, c.size, words,
                        { head: 0, tail: 1, sequence: 2, lock: 3 });
    const from = { words, index: ring.tail };
    const state = { lastSeq: -1 };
    const got = [];

    for (const op of c.ops) {
      if (op.write) {
        const ok = ringWrite(ring, op.payload, op.sourceId);
        assert.equal(ok, op.expect === "ok",
          `${c.name}: write src=${op.sourceId} len=${op.payload.length} should be ${op.expect}`);
      } else {
        ringDrain(ring, from, state, op.max, (origin, offset, length, sequence) => {
          got.push({ seq: sequence, sourceId: origin,
                     payload: toHex(ring.bytes.subarray(offset, offset + length)) });
        });
      }
    }

    assert.deepEqual(got, c.msgs, `${c.name}: the frames delivered`);
    assert.equal(words[ring.head], c.head, `${c.name}: head`);
    assert.equal(words[ring.tail], c.tail, `${c.name}: tail`);
    assert.equal(toHex(ring.bytes), c.image, `${c.name}: the ring's bytes`);
    assert.equal(words[ring.lock], 0, `${c.name}: the lock is free afterwards`);
  }
});
