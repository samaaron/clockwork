#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The linker's map, reduced to what names static memory: <module>.map (wasm-ld,
// -Wl,--Map=) in, <module>.symbols.json out. The dev tools read the json to draw
// the program's static data by symbol — the compiler's statement of what each
// byte is, beside the engine's table of what it shares. Nothing is inferred:
// a byte the map does not name stays unnamed.
//
//   node scripts/static-map.mjs dist/wasm/clockwork.map [dist/wasm/clockwork.symbols.json]
//
// The json: { sections: [{ kind, from, to }], symbols: [{ kind, from, size, name }] },
// both in address order; kind is "rodata" (constants), "data" (initialised
// globals) or "bss" (zero-initialised globals). A symbol's name is demangled
// (C++ and Rust) when llvm-cxxfilt is beside emcc, as the mangled name
// otherwise.
import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";

/** Parse a wasm-ld map into sections and symbols; exported for the test. */
export function reduceMap(text) {
  const sections = [];
  const symbols = [];
  let kind = null;          // the output section being listed
  let pending = null;       // an input section awaiting its symbol line
  // the columns: the header says where an output section's name, an input section's and a symbol's begin
  let inCol = 34, symCol = 42;
  const line = /^\s*([0-9a-f]+|-)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+(.*)$/;
  for (const raw of text.split("\n")) {
    const h = /^\s*Addr\s+Off\s+Size\s+Out\s+In\s+Symbol/.exec(raw);
    if (h) { inCol = raw.indexOf("In"); symCol = raw.indexOf("Symbol"); continue; }
    const m = line.exec(raw);
    if (!m) continue;
    const [, addr, , size, rest] = m;
    const pos = raw.length - rest.length;   // where the name begins: which column it is in
    const out = rest.trim();
    if (pos < inCol || addr === "-") {   // an output section
      pending = null;
      kind = { ".rodata": "rodata", ".data": "data", ".bss": "bss" }[out] ?? null;
      if (kind) sections.push({ kind, from: parseInt(addr, 16), to: parseInt(addr, 16) + parseInt(size, 16) });
      continue;
    }
    if (!kind) continue;
    const input = /^(\S+)\((\.(?:rodata|data|bss)(?:\.(.*))?)\)$/.exec(out);
    if (input) {   // an input section: its symbol may follow on the next line; else it is named by its suffix
      if (pending) symbols.push(pending);
      const n = parseInt(size, 16);
      pending = n > 0 ? { kind, from: parseInt(addr, 16), size: n, name: input[3] ?? "" } : null;
      continue;
    }
    if (pending && pos >= symCol && out) {   // the symbol line under its input section
      if (parseInt(addr, 16) === pending.from) pending.name = out;
      symbols.push(pending);
      pending = null;
    }
  }
  if (pending) symbols.push(pending);
  symbols.sort((a, b) => a.from - b.from);
  return { sections, symbols };
}

/** Demangle names in place, with llvm-cxxfilt beside emcc; mangled names stay if it is not there. */
export function demangle(symbols) {
  const emcc = spawnSync("sh", ["-c", "command -v emcc"], { encoding: "utf8" }).stdout.trim();
  if (!emcc) return false;
  const real = fs.realpathSync(emcc);
  const candidates = [path.join(path.dirname(real), "..", "libexec", "llvm", "bin", "llvm-cxxfilt"), path.join(path.dirname(real), "llvm-cxxfilt")];
  const filt = candidates.find((c) => fs.existsSync(c));
  if (!filt) return false;
  const names = symbols.map((s) => s.name);
  const r = spawnSync(filt, [], { input: names.join("\n") + "\n", encoding: "utf8", maxBuffer: 1 << 26 });
  if (r.status !== 0) return false;
  const out = r.stdout.split("\n");
  symbols.forEach((s, i) => { if (out[i]) s.name = out[i]; });
  return true;
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(new URL(import.meta.url).pathname)) {
  const [mapPath, outPath] = process.argv.slice(2);
  if (!mapPath) { console.error("usage: static-map.mjs <module>.map [<module>.symbols.json]"); process.exit(2); }
  const out = outPath ?? mapPath.replace(/\.map$/, "") + ".symbols.json";
  const reduced = reduceMap(fs.readFileSync(mapPath, "utf8"));
  demangle(reduced.symbols);
  fs.writeFileSync(out, JSON.stringify(reduced));
  const bytes = reduced.symbols.reduce((n, s) => n + s.size, 0);
  console.log(`${path.basename(out)}: ${reduced.symbols.length} symbols, ${(bytes / 1048576).toFixed(2)} MB, in ${reduced.sections.map((s) => s.kind).join(" ")}`);
}
