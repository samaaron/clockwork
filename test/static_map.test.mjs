// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// scripts/static-map.mjs against a map in wasm-ld's shape: the three sections
// by address, a symbol named by the line under its input section, one named
// by its input section's suffix when no line follows, the merged string pool
// left unnamed, and nothing from the code sections.
import { test } from "node:test";
import assert from "node:assert/strict";
import { reduceMap } from "../scripts/static-map.mjs";

const MAP = `    Addr      Off     Size Out     In      Symbol
       -        8      39a TYPE
     460   1957fa    10e28 .rodata
     478   195812     5e37         <internal>:(.rodata)
    92f8   19e8f6     1090         libc-mt.a(log.o):(.rodata.__log_data)
    92f8   19e8f6     1090                 __log_data
   11288   1a6626      b10 .data
   11288   1a6629        4         libdlmalloc-mt.a(sbrk.o):(.data.sbrk_val)
   11288   1a6629        4                 sbrk_val
   11da0   195797   75b949 .bss
   131a0        0   616a00         engine.lto.o:(.bss.arena_storage)
   131a0        0   616a00                 arena_storage
  629ba0        0    10000         engine.lto.o:(.bss.static_audio_bus)
  629ba0        0    10000                 static_audio_bus
  639ba0        0        0         engine.lto.o:(.bss.nothing)
  76d6e0   19ddd3    36168 CODE
  76d6e0   19ddd3    36168         engine.lto.o:(.text.dsp_new)
`;

test("sections and symbols come out in address order, named by the symbol line or the input section", () => {
  const { sections, symbols } = reduceMap(MAP);
  assert.deepEqual(sections, [
    { kind: "rodata", from: 0x460, to: 0x460 + 0x10e28 },
    { kind: "data", from: 0x11288, to: 0x11288 + 0xb10 },
    { kind: "bss", from: 0x11da0, to: 0x11da0 + 0x75b949 },
  ]);
  assert.deepEqual(symbols.map((s) => [s.kind, s.from, s.size, s.name]), [
    ["rodata", 0x478, 0x5e37, ""],                  // the merged string pool: the map names nothing
    ["rodata", 0x92f8, 0x1090, "__log_data"],
    ["data", 0x11288, 4, "sbrk_val"],
    ["bss", 0x131a0, 0x616a00, "arena_storage"],
    ["bss", 0x629ba0, 0x10000, "static_audio_bus"],
  ]);   // the empty .bss.nothing is dropped; the code section is not static data
});
