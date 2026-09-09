// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * Fail the build on an import that can never resolve.
 *
 * esbuild emits `import-is-undefined` when a module imports a name its target
 * does not export. That is normally a warning; here it is an error, because
 * in this codebase it has twice meant dead code that looked alive:
 *
 *   - the worklet wrote seven metrics through MetricsOffsets names that a
 *     rename had already replaced with ENGINE_*. Indexing a typed array with
 *     `undefined` neither throws nor writes, so the counters simply stayed at
 *     zero.
 *   - metrics_reader.js wrote four more into CTX_BUFFER_POOL_* after those
 *     offsets became the guest range.
 *
 * Eleven silently dead writes, none of which any test could see, because a
 * metric reading zero looks exactly like a metric with nothing to report.
 *
 * It also catches a module that does not PARSE — which is how
 * midi_manager.js shipped for months with one import statement pasted into
 * the middle of another.
 */
import { build } from "esbuild";
import { readdirSync, statSync, existsSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");

/** Every .js under js/, which is every entry point the browser can reach. */
function entryPoints(dir, out = []) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) entryPoints(p, out);
    else if (name.endsWith(".js")) out.push(p);
  }
  return out;
}

const files = entryPoints(join(ROOT, "js"));
if (files.length === 0) {
  console.error("check-js-imports: found no JS to check — is js/ missing?");
  process.exit(1);
}

let failed = 0;
for (const file of files) {
  try {
    const result = await build({
      entryPoints: [file],
      bundle: true,
      write: false,
      format: "esm",
      logLevel: "silent",
      define: { __DEV__: "true" },
      // The wasm artifact is fetched at runtime, not bundled.
      external: ["*.wasm"],
    });
    for (const w of result.warnings) {
      if (w.id === "import-is-undefined") {
        const loc = w.location;
        console.error(`DEAD IMPORT  ${loc?.file}:${loc?.line}  ${w.text}`);
        failed++;
      }
    }
  } catch (e) {
    const errs = e.errors ?? [{ text: String(e) }];
    /*
     * A module that imports a GENERATED artifact under dist/ cannot bundle
     * until build:web has produced it. That is build order, not a defect —
     * the MIDI and gamepad managers import their wasm-bindgen output — so it
     * is reported and not counted. Anything else stopping the bundle IS a
     * defect: a parse error, a typo'd path, a module that no longer exists.
     */
    const onlyMissingArtifacts = errs.every((err) =>
      /Could not resolve "[^"]*\/dist\//.test(err.text));
    if (onlyMissingArtifacts) {
      console.warn(`needs build:web   ${file}`);
      console.warn(`                  ${errs[0].text}`);
      continue;
    }
    console.error(`WILL NOT BUILD  ${file}`);
    for (const err of errs) console.error(`                ${err.text}`);
    failed++;
  }
}

if (failed) {
  console.error(`\ncheck-js-imports: ${failed} problem(s). These do not throw at`
              + ` runtime — they silently do nothing, which is why they are errors here.`);
  process.exit(1);
}
console.log(`check-js-imports: ${files.length} entry points, no dead imports.`);
