// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * Clockwork's software bill of materials, one per build target.
 *
 * What a build contains depends on the target: the Rust crates cargo resolves
 * for that triple and the features CMake's options turn on, the npm packages
 * the web client ships, and the vendored C and C++ (src/vendor,
 * src/native/vendor, external, smoothie's JUCE, what CMake fetches) that the
 * target's build options compile in. The crates and packages come from the
 * lockfiles through `cargo metadata` and package-lock.json; the vendored code
 * has no package manager, so docs/components.cdx.json lists it by hand with
 * the targets and CMake options that include each entry, and this script
 * keeps that list true.
 *
 *   node scripts/sbom.mjs check
 *       Fails when vendored code or a FetchContent_Declare (in a CMakeLists.txt
 *       or an external/fetch-*.cmake) has no entry in the
 *       list, when an entry's path is gone, when an entry's version is not
 *       the GIT_TAG the build fetches or not what the vendored source itself
 *       says (clockwork:version_file and clockwork:version_pattern name where
 *       to read it), when an entry names no targets, or when it names a CMake
 *       option that does not exist. An unlisted vendored library makes the
 *       SBOM incomplete.
 *
 *       Code we forked and now maintain as our own goes in the list as an
 *       ancestor (metadata.component.pedigree.ancestors), not as a component.
 *       check checks its path, version and upstream commit like any entry,
 *       target puts it in each document's metadata, and scan skips it.
 *
 *   node scripts/sbom.mjs target <name>|all [outdir] [version]
 *       Writes CycloneDX 1.6 JSON for one CI target (linux-x64, linux-arm64,
 *       linux-x86, macos-arm64, macos-x64, windows-x64, web) or all of them,
 *       as <outdir>/clockwork-<target>.cdx.json (outdir defaults to
 *       build/sbom). Each is the default build of that target: CMake's option
 *       defaults, read from CMakeLists.txt, decide the cargo features and
 *       which vendored entries are compiled in. A build with other options is
 *       a different product and gets its own document.
 *
 *       The documents are reproducible from a commit: the list, the lockfiles
 *       and this script are all in the tree, so a project that pins clockwork
 *       as a submodule regenerates the document for its pin and references it
 *       from its own SBOM rather than copying its contents.
 *
 *   node scripts/sbom.mjs scan <document>...
 *       Looks every component of the documents up in the advisory databases
 *       and fails on any advisory not accepted by docs/vex.cdx.json. Crates
 *       and npm packages are looked up by purl; vendored code by the upstream
 *       commit it was taken from (clockwork:upstream_commit; for a fork, the
 *       commit it forked from), which OSV matches against the git ranges of
 *       CVE records; code with no public repository (the ASIO SDK) by a
 *       keyword search of the NVD (clockwork:advisory_search). A component
 *       with none of these fails the scan: the vendored code is exactly what
 *       a lockfile scanner never sees, so nothing is skipped. For a library
 *       CMake fetches, the recorded commit is also checked against what the
 *       tag points at today. Needs the network.
 *
 * Every command takes `--root <dir>` to run on another project's list
 * (project() below says what that list states). A project that pins other
 * projects as submodules (clockwork, or a product built on it) gets their
 * documents for each target from the pins, written beside its own, and lists
 * each direct pin as a component referring to its document; the pins' own
 * pins are reached the same way through them. Its scan applies the VEX
 * statements of every project in the chain.
 *
 * Needs node and cargo (for `cargo metadata`, clockwork's tree only); no
 * other tool.
 */
import { execFileSync } from "node:child_process";
import { randomUUID } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, realpathSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");

// Where clockwork's hand-carried code lives. Each entry directly under one of
// these must be covered by a component's `clockwork:path`.
const VENDOR_DIRS = ["src/vendor", "src/native/vendor", "external"];
// clockwork's own files that sit beside vendored code.
const OWN = [/\.cmake$/, /^CLOCKWORK.*\.md$/i, /^\./];
// Not descended into when looking for CMakeLists.txt.
const SKIP = new Set([".git", "node_modules", "target", "build", "dist"]);

// The CI targets (ci.yml's native matrix, and the web build). `kind` is what
// a vendored entry's clockwork:targets names; windows-x64 is also "windows".
const TARGETS = {
  "linux-x64":   { kind: "native", triple: "x86_64-unknown-linux-gnu" },
  "linux-arm64": { kind: "native", triple: "aarch64-unknown-linux-gnu" },
  "linux-x86":   { kind: "native", triple: "i686-unknown-linux-gnu" },
  "macos-arm64": { kind: "native", triple: "aarch64-apple-darwin" },
  "macos-x64":   { kind: "native", triple: "x86_64-apple-darwin" },
  "windows-x64": { kind: "native", triple: "x86_64-pc-windows-msvc", windows: true },
  // The engine's Rust umbrella compiled into the wasm module, plus the MIDI
  // and gamepad subsystems built as wasm-bindgen modules (build-web-subsystems.sh).
  "web":         { kind: "web", triple: "wasm32-unknown-emscripten", subsystems: "wasm32-unknown-unknown" },
};

const prop = (c, name) => c.properties?.find((p) => p.name === `clockwork:${name}`)?.value;

// A project: the tree whose docs/components.cdx.json is read. clockwork by
// default; `--root <dir>` names another tree, which states in its list's
// metadata properties (each comma-separated where it is a list):
//   clockwork:vendor-dirs    where its vendored code lives ("" for nowhere)
//   clockwork:option-prefix  the prefix of its CMake options
//   clockwork:skip-dirs      directories not searched for CMakeLists.txt
//   clockwork:pins           submodules that are projects with lists of their
//                            own (clockwork, or a product built on it); they
//                            describe themselves, so their trees are skipped
//                            here too
export function project(root = ROOT) {
  const list = JSON.parse(readFileSync(join(root, "docs", "components.cdx.json"), "utf8"));
  const setting = (name) => prop(list.metadata ?? {}, name);
  const csv = (v) => (v === undefined ? null : v.split(",").map((s) => s.trim()).filter(Boolean));
  const pins = csv(setting("pins")) ?? [];
  return {
    root,
    list,
    name: list.metadata?.component?.name ?? "clockwork",
    vendorDirs: csv(setting("vendor-dirs")) ?? VENDOR_DIRS,
    optionPrefix: setting("option-prefix") ?? "CLOCKWORK_",
    skipDirs: new Set([...(csv(setting("skip-dirs")) ?? []), ...pins]),
    pins,
    // The Rust workspace a build links crates from: clockwork's.
    cargo: existsSync(join(root, "rust", "clockwork-native", "Cargo.toml")),
  };
}

function cmakeFiles(proj, dir = proj.root, out = []) {
  for (const name of readdirSync(dir)) {
    if (SKIP.has(name) || name.startsWith("build")) continue;
    const p = join(dir, name);
    if (proj.skipDirs.has(relative(proj.root, p))) continue;
    if (statSync(p).isDirectory()) cmakeFiles(proj, p, out);
    // A fetch shared between the main tree and the fuzz build lives in
    // external/fetch-<name>.cmake, read like a CMakeLists.txt.
    else if (name === "CMakeLists.txt" || /^fetch-.*\.cmake$/.test(name)) out.push(p);
  }
  return out;
}

// FetchContent_Declare(<name> ... GIT_REPOSITORY <url> GIT_TAG <tag> ...)
function fetches(proj) {
  const found = [];
  for (const file of cmakeFiles(proj)) {
    const text = readFileSync(file, "utf8");
    for (const m of text.matchAll(/^\s*FetchContent_Declare\(\s*(\S+)([^)]*)\)/gm)) {
      const tag = /GIT_TAG\s+(\S+)/.exec(m[2])?.[1];
      const url = /GIT_REPOSITORY\s+(\S+)/.exec(m[2])?.[1];
      found.push({ name: m[1], tag, url, file: relative(proj.root, file) });
    }
  }
  return found;
}

// CMake's options and their defaults: option(NAME "help" ON|OFF), from the
// top-level CMakeLists.txt. What a default build turns on.
function cmakeOptions(proj) {
  const options = new Map();
  if (!existsSync(join(proj.root, "CMakeLists.txt"))) return options;
  const text = readFileSync(join(proj.root, "CMakeLists.txt"), "utf8");
  const re = new RegExp(`^\\s*option\\(\\s*(${proj.optionPrefix}[A-Z0-9_]+)\\s+"(?:[^"\\\\]|\\\\.)*"\\s+(ON|OFF)\\s*\\)`, "gm");
  for (const m of text.matchAll(re)) {
    options.set(m[1], m[2] === "ON");
  }
  return options;
}

// The version a vendored file states: the pattern's capture groups, joined
// with dots when there are several (a header that spells MAJOR, MINOR and
// BUILD on three lines), except that a group starting with "-" or "+" is a
// pre-release or build suffix and is appended as it is, and an empty group is
// left out. null when the pattern matches nothing.
function versionIn(proj, file, pattern) {
  const full = join(proj.root, file);
  if (!existsSync(full)) return null;
  const m = new RegExp(pattern, "m").exec(readFileSync(full, "utf8"));
  if (!m) return null;
  return m.slice(1).filter((g) => g !== "").reduce((v, g) => (/^[-+]/.test(g) ? v + g : v ? `${v}.${g}` : g), "");
}

function check(proj = project()) {
  const { components } = proj.list;
  const options = cmakeOptions(proj);
  const problems = [];
  const paths = components.map((c) => prop(c, "path")).filter(Boolean);
  // Ancestors: code we forked and maintain as our own. Their path, version and
  // upstream commit are checked like any entry's, but they have no targets and
  // scan skips them.
  const ancestors = proj.list.metadata?.component?.pedigree?.ancestors ?? [];

  for (const c of [...components, ...ancestors]) {
    const p = prop(c, "path");
    if (p && !existsSync(join(proj.root, p))) problems.push(`${c.name}: ${p} does not exist`);
    if (!p && !prop(c, "fetch")) problems.push(`${c.name}: neither clockwork:path nor clockwork:fetch`);
    // A vendored entry's version is what its source says, read every time;
    // code with no release versions is versioned by its upstream commit
    // (clockwork:version_from = upstream_commit).
    const vfile = prop(c, "version_file");
    const vpat = prop(c, "version_pattern");
    const commit = prop(c, "upstream_commit");
    if (prop(c, "version_from") === "upstream_commit") {
      if (!commit || c.version !== commit.slice(0, 7)) problems.push(`${c.name}: versioned by its upstream commit, but ${c.version} is not the first seven characters of it`);
    } else if (p && !prop(c, "fetch") && !(vfile && vpat)) {
      problems.push(`${c.name}: vendored, but no clockwork:version_file and clockwork:version_pattern to read its version from`);
    } else if (vfile && vpat) {
      const found = versionIn(proj, vfile, vpat);
      if (found === null) problems.push(`${c.name}: ${vpat} matches nothing in ${vfile}`);
      else if (found !== c.version) problems.push(`${c.name}: ${vfile} says ${found}, the list says ${c.version}`);
    }
    // A fork whose source records the upstream commit it is synced to
    // (clockwork:commit_file, clockwork:commit_pattern): the list's commit
    // must be that one.
    const cfile = prop(c, "commit_file");
    const cpat = prop(c, "commit_pattern");
    if (cfile && cpat) {
      const recorded = versionIn(proj, cfile, cpat);
      if (recorded === null) problems.push(`${c.name}: ${cpat} matches nothing in ${cfile}`);
      else if (!commit || !commit.startsWith(recorded)) problems.push(`${c.name}: ${cfile} records upstream commit ${recorded}, the list records ${commit ?? "none"}`);
    }
    if (commit && !/^[0-9a-f]{40}$/.test(commit)) problems.push(`${c.name}: clockwork:upstream_commit is not a full commit id`);
    if (ancestors.includes(c)) continue;
    // Which builds compile it in: targets, and the options that include or exclude it.
    const targets = prop(c, "targets");
    if (!targets) problems.push(`${c.name}: no clockwork:targets (native, web, windows, or none)`);
    else for (const t of targets.split(",")) {
      if (!["native", "web", "windows", "none"].includes(t.trim())) problems.push(`${c.name}: unknown target "${t}" in clockwork:targets`);
    }
    for (const key of ["option", "unless"]) {
      const o = prop(c, key);
      if (o && !options.has(o)) problems.push(`${c.name}: clockwork:${key} names ${o}, which CMakeLists.txt does not define`);
    }
    // How `scan` will look it up.
    if (!lookup(c)) problems.push(`${c.name}: no clockwork:upstream_commit or clockwork:advisory_search, so scan could not look it up`);
  }

  for (const dir of proj.vendorDirs) {
    for (const name of readdirSync(join(proj.root, dir))) {
      if (OWN.some((re) => re.test(name))) continue;
      const p = `${dir}/${name}`;
      // Covered by an entry for it, for something inside it, or for the
      // vendor directory itself when that is one component. An entry for a
      // directory holding a vendor directory (a fork that carries other
      // projects' code) does not cover what is vendored inside it.
      const covered = paths.some((q) => q === p || q.startsWith(`${p}/`) || q === dir)
        || components.some((c) => c.pedigree?.patches?.some((x) => x.diff?.url === p));
      if (!covered) problems.push(`${p}: vendored, but not in docs/components.cdx.json`);
    }
  }

  for (const f of fetches(proj)) {
    const c = components.find((x) => prop(x, "fetch") === f.name);
    if (!c) { problems.push(`${f.file}: FetchContent_Declare(${f.name}) is not in docs/components.cdx.json`); continue; }
    if (f.tag && c.version !== f.tag) problems.push(`${f.name}: the build fetches ${f.tag}, the list says ${c.version}`);
    const vcs = c.externalReferences?.find((r) => r.type === "vcs")?.url;
    if (f.url && vcs && vcs.replace(/\.git$/, "") !== f.url.replace(/\.git$/, "")) {
      problems.push(`${f.name}: the build fetches ${f.url}, the list says ${vcs}`);
    }
  }

  if (problems.length) {
    for (const p of problems) console.error(`sbom: ${p}`);
    process.exit(1);
  }
  console.log(`sbom: ${proj.name}: ${components.length} components${ancestors.length ? `, ${ancestors.length} ancestor(s)` : ""}, every vendored path and fetch accounted for`);
}

// ---------------------------------------------------------------------------
// Per-target documents
// ---------------------------------------------------------------------------

// The cargo features CMake turns on for a default build (CMakeLists.txt,
// CLOCKWORK_RUST_FEATURES), from the option defaults.
function cargoFeatures(options, windows) {
  const f = [];
  if (options.get("CLOCKWORK_MIDI")) { f.push("midi"); if (windows) f.push("winrt"); }
  if (options.get("CLOCKWORK_GAMEPAD")) f.push("gamepad");
  if (options.get("CLOCKWORK_OSC")) f.push("osc", "comms");
  if (options.get("CLOCKWORK_SCHEDULER")) f.push("schedule");
  return f;
}

// cargo's licence field is an SPDX expression, except the legacy "A/B" form.
function cargoLicence(pkg) {
  if (pkg.license) return pkg.license.includes("/") && !pkg.license.includes(" ") ? pkg.license.split("/").join(" OR ") : pkg.license;
  if (pkg.license_file) return `LicenseRef-${pkg.name}-license-file`;
  return undefined;
}

// The third-party crates a build of `manifest` compiles for `triple`: the
// resolve graph walked from its root over normal and build dependencies (not
// dev), as cargo resolves it for that platform. Workspace members are
// clockwork's own and are not components of it.
function cargoComponents(proj, manifest, triple, features, members) {
  const args = ["metadata", "--format-version", "1", "--locked", "--manifest-path", join(proj.root, manifest), "--filter-platform", triple];
  if (features) args.push("--no-default-features", "--features", features.join(","));
  const meta = JSON.parse(execFileSync("cargo", args, { encoding: "utf8", maxBuffer: 64 << 20 }));
  const byId = new Map(meta.packages.map((p) => [p.id, p]));
  const nodes = new Map(meta.resolve.nodes.map((n) => [n.id, n]));
  const seen = new Set();
  const queue = [meta.resolve.root];
  while (queue.length) {
    const id = queue.shift();
    if (seen.has(id)) continue;
    seen.add(id);
    for (const d of nodes.get(id)?.deps ?? []) {
      if (d.dep_kinds.some((k) => k.kind === null || k.kind === "build")) queue.push(d.pkg);
    }
  }
  const out = [];
  for (const id of seen) {
    const p = byId.get(id);
    if (members.has(p.name)) continue;
    const licence = cargoLicence(p);
    out.push({
      type: "library",
      "bom-ref": `pkg:cargo/${p.name}@${p.version}`,
      name: p.name,
      version: p.version,
      purl: `pkg:cargo/${p.name}@${p.version}`,
      ...(licence ? { licenses: [{ expression: licence }] } : {}),
      ...(p.repository ? { externalReferences: [{ type: "vcs", url: p.repository }] } : {}),
    });
  }
  return out;
}

// The packages the web client ships: package.json's `dependencies`, resolved
// through package-lock.json, transitively; never devDependencies.
function npmComponents(proj) {
  if (!existsSync(join(proj.root, "package.json"))) return [];
  const pkg = JSON.parse(readFileSync(join(proj.root, "package.json"), "utf8"));
  const lock = JSON.parse(readFileSync(join(proj.root, "package-lock.json"), "utf8"));
  const out = [];
  const seen = new Set();
  const queue = Object.keys(pkg.dependencies ?? {});
  while (queue.length) {
    const name = queue.shift();
    if (seen.has(name)) continue;
    seen.add(name);
    const entry = lock.packages[`node_modules/${name}`];
    if (!entry) throw new Error(`package-lock.json has no entry for ${name}`);
    const purl = `pkg:npm/${name.replace(/^@/, "%40")}@${entry.version}`;
    out.push({
      type: "library",
      "bom-ref": purl,
      name,
      version: entry.version,
      purl,
      ...(entry.license ? { licenses: [{ expression: entry.license }] } : {}),
    });
    queue.push(...Object.keys(entry.dependencies ?? {}));
  }
  return out;
}

// The hand-listed entries a default build of `target` compiles in: the
// target kind is among the entry's targets, its including option (if any)
// defaults ON, and its excluding option (if any) defaults OFF.
function vendoredComponents(list, target, options) {
  return list.components.filter((c) => {
    const targets = (prop(c, "targets") ?? "").split(",").map((t) => t.trim());
    const kinds = [target.kind, ...(target.windows ? ["windows"] : [])];
    if (!targets.some((t) => kinds.includes(t))) return false;
    const option = prop(c, "option");
    if (option && !options.get(option)) return false;
    const unless = prop(c, "unless");
    if (unless && options.get(unless)) return false;
    return true;
  });
}

function workspaceMembers(proj) {
  if (!proj.cargo) return new Set();
  const meta = JSON.parse(execFileSync("cargo", ["metadata", "--format-version", "1", "--no-deps", "--locked",
    "--manifest-path", join(proj.root, "rust", "Cargo.toml")], { encoding: "utf8", maxBuffer: 64 << 20 }));
  return new Set(meta.packages.map((p) => p.name));
}

// A pinned project as a component of the project that pins it: versioned by
// the pinned commit, and referring by BOM-Link to its document for the target.
function pinnedComponent(pinProj, commit, doc, file) {
  const subject = pinProj.list.metadata.component;
  return {
    type: "library",
    "bom-ref": `${pinProj.name}@${commit}`,
    name: pinProj.name,
    version: commit.slice(0, 7),
    ...(subject.licenses ? { licenses: subject.licenses } : {}),
    externalReferences: [
      ...(subject.externalReferences ?? []),
      { type: "bom", url: `urn:cdx:${doc.serialNumber.replace(/^urn:uuid:/, "")}/${doc.version}`, comment: basename(file) },
    ],
    properties: [{ name: "clockwork:upstream_commit", value: commit }],
  };
}

function targetDocument(proj, name, version, options, members, pinned = []) {
  const target = TARGETS[name];
  if (!target) throw new Error(`unknown target ${name}; one of ${Object.keys(TARGETS).join(", ")}`);
  const features = proj.cargo ? cargoFeatures(options, target.windows) : [];
  let crates = proj.cargo ? cargoComponents(proj, "rust/clockwork-native/Cargo.toml", target.triple, features, members) : [];
  if (proj.cargo && target.subsystems) {
    for (const crate of ["clockwork-midi", "clockwork-gamepad"]) {
      crates = crates.concat(cargoComponents(proj, `rust/${crate}/Cargo.toml`, target.subsystems, null, members));
    }
  }
  const npm = target.kind === "web" ? npmComponents(proj) : [];
  const vendored = vendoredComponents(proj.list, target, options);
  vendored.push(...pinned);

  // One entry per name@version. The hand-listed entry wins: a cargo path
  // dependency such as the midir fork reaches cargo with no licence, no
  // upstream and no word of what clockwork changed.
  const byKey = new Map();
  for (const c of [...crates, ...npm, ...vendored]) byKey.set(`${c.name}@${c.version}`, c);
  const components = [...byKey.values()].sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version));

  const subject = { ...proj.list.metadata.component, ...(version ? { version } : {}) };
  return {
    $schema: "http://cyclonedx.org/schema/bom-1.6.schema.json",
    bomFormat: "CycloneDX",
    specVersion: "1.6",
    serialNumber: `urn:uuid:${randomUUID()}`,
    version: 1,
    metadata: {
      timestamp: new Date().toISOString(),
      component: subject,
      properties: [
        { name: "clockwork:target", value: name },
        { name: "clockwork:triple", value: target.triple },
        ...(proj.cargo ? [{ name: "clockwork:cargo-features", value: features.join(",") }] : []),
        { name: "clockwork:build-options", value: "CMakeLists.txt defaults" },
      ],
    },
    components,
  };
}

// Writes <outdir>/<project>-<target>.cdx.json for each target, and first the
// documents of every project it pins, recursively. Each document lists the
// project's direct pins as components referring to theirs. A project reached
// twice at one commit (a product pinning clockwork and a product that pins
// clockwork too) is written once; reached at two commits, the run stops,
// since both would be written to the same file. Returns each target's
// document and the file it was written to.
function target(proj, which, outdir, version, done = new Map()) {
  const options = cmakeOptions(proj);
  const members = workspaceMembers(proj);
  const names = which === "all" ? Object.keys(TARGETS) : [which];
  const pins = proj.pins.map((path) => {
    const root = join(proj.root, path);
    const commit = execFileSync("git", ["-C", root, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
    const pin = project(root);
    const seen = done.get(pin.name);
    if (seen && seen.commit !== commit) {
      throw new Error(`${pin.name} is pinned at ${seen.commit.slice(0, 7)} and at ${commit.slice(0, 7)} (${path})`);
    }
    const docs = seen ? seen.docs : target(pin, which, outdir, commit.slice(0, 7), done);
    done.set(pin.name, { commit, docs });
    return { pin, commit, docs };
  });
  mkdirSync(outdir, { recursive: true });
  const written = {};
  for (const name of names) {
    const pinned = pins.map(({ pin, commit, docs }) => pinnedComponent(pin, commit, docs[name].doc, docs[name].file));
    const doc = targetDocument(proj, name, version, options, members, pinned);
    const out = join(outdir, `${proj.name}-${name}.cdx.json`);
    writeFileSync(out, JSON.stringify(doc, null, 2) + "\n");
    written[name] = { doc, file: out };
    const kinds = { cargo: 0, npm: 0, vendored: 0 };
    for (const c of doc.components) kinds[c.purl?.startsWith("pkg:cargo/") ? "cargo" : c.purl?.startsWith("pkg:npm/") ? "npm" : "vendored"]++;
    console.log(`sbom: ${relative(proj.root, out)}: ${doc.components.length} components (${kinds.cargo} crates, ${kinds.npm} npm, ${kinds.vendored} vendored)`);
  }
  return written;
}

// ---------------------------------------------------------------------------
// Advisories
// ---------------------------------------------------------------------------

// The analysis states under which a VEX statement accepts an advisory.
const ACCEPTED = new Set(["not_affected", "false_positive", "resolved"]);

// How a component is looked up, or null when it cannot be.
export function lookup(c) {
  if (/^pkg:(cargo|npm)\//.test(c.purl ?? "")) return { kind: "purl", key: c.purl };
  const commit = prop(c, "upstream_commit");
  if (commit) return { kind: "commit", key: commit, fork: prop(c, "modified") === "yes" };
  const search = prop(c, "advisory_search");
  if (search) return { kind: "search", key: search };
  return null;
}

// OSV, one request for every purl and commit: the advisory ids per query.
async function osvQuery(queries) {
  const res = await fetch("https://api.osv.dev/v1/querybatch", {
    method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify({ queries }),
  });
  if (!res.ok) throw new Error(`OSV: HTTP ${res.status}`);
  const { results } = await res.json();
  return results.map((r, i) => {
    if (r.next_page_token) throw new Error(`OSV: more than one page of advisories for ${JSON.stringify(queries[i])}`);
    return (r.vulns ?? []).map((v) => v.id);
  });
}

// The NVD's keyword search: the CVE ids that mention the words.
async function nvdSearch(keyword) {
  const res = await fetch(`https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=${encodeURIComponent(keyword)}`);
  if (!res.ok) throw new Error(`NVD: HTTP ${res.status}`);
  const body = await res.json();
  return (body.vulnerabilities ?? []).map((v) => v.cve.id);
}

// What a tag points at today, so a bumped GIT_TAG with a stale recorded
// commit is caught.
function tagCommit(url, tag) {
  const out = execFileSync("git", ["ls-remote", "--tags", url, `refs/tags/${tag}`, `refs/tags/${tag}^{}`], { encoding: "utf8" });
  const lines = out.trim().split("\n").filter(Boolean).map((l) => l.split("\t"));
  return (lines.find(([, ref]) => ref.endsWith("^{}")) ?? lines[0])?.[0];
}

// The project's VEX statements and those of every project it pins,
// recursively, each tree once: the pins' documents are scanned with its own.
export function readVex(proj = project(), seen = new Set()) {
  const key = realpathSync(proj.root);
  if (seen.has(key)) return { vulnerabilities: [] };
  seen.add(key);
  const file = join(proj.root, "docs", "vex.cdx.json");
  const own = existsSync(file) ? JSON.parse(readFileSync(file, "utf8")).vulnerabilities ?? [] : [];
  const pinned = proj.pins.flatMap((path) => readVex(project(join(proj.root, path)), seen).vulnerabilities);
  return { vulnerabilities: [...own, ...pinned] };
}

// Scans the documents. Returns the report lines and the problems; the
// command exits non-zero on any problem. The lookups are parameters so the
// tests can stand in for the network.
export async function scan(files, { osv = osvQuery, nvd = nvdSearch, tag = tagCommit, vex = readVex(), root = ROOT } = {}) {
  const problems = [];
  const lines = [];
  // Distinct components across the documents, by name and version.
  const components = new Map();
  for (const file of files) {
    const doc = JSON.parse(readFileSync(file, "utf8"));
    for (const c of doc.components) {
      const key = `${c.name}@${c.version}`;
      if (!components.has(key)) components.set(key, { c, docs: [] });
      components.get(key).docs.push(relative(root, file));
    }
  }

  const entries = [...components.values()].sort((a, b) => a.c.name.localeCompare(b.c.name) || a.c.version.localeCompare(b.c.version));
  const queries = [];
  for (const e of entries) {
    e.how = lookup(e.c);
    if (!e.how) { problems.push(`${e.c.name} ${e.c.version}: no purl, clockwork:upstream_commit or clockwork:advisory_search; not looked up`); continue; }
    if (e.how.kind === "purl") { e.query = queries.length; queries.push({ package: { purl: e.how.key } }); }
    if (e.how.kind === "commit") { e.query = queries.length; queries.push({ commit: e.how.key }); }
  }
  const results = queries.length ? await osv(queries) : [];
  for (const e of entries) {
    if (!e.how) continue;
    e.ids = e.how.kind === "search" ? await nvd(e.how.key) : results[e.query];
    // A fetched library: the recorded commit must still be the tag.
    const fetched = prop(e.c, "fetch");
    if (fetched && e.how.kind === "commit") {
      const url = e.c.externalReferences?.find((r) => r.type === "vcs")?.url;
      const now = tag(url, e.c.version);
      if (now !== e.how.key) problems.push(`${e.c.name}: ${e.c.version} is ${now ?? "no tag"} at ${url}, the list records ${e.how.key}`);
    }
  }

  // The VEX statements: each accepts one advisory for one component.
  const statements = vex.vulnerabilities ?? [];
  const used = new Set();
  let open = 0, accepted = 0;
  const width = Math.max(...entries.map((e) => `${e.c.name} ${e.c.version}`.length)) + 2;
  for (const e of entries) {
    if (!e.ids) continue;
    const ref = e.c["bom-ref"];
    const verdicts = e.ids.map((id) => {
      const s = statements.find((v) => v.id === id && v.affects?.some((a) => a.ref === ref) && ACCEPTED.has(v.analysis?.state));
      if (s) { used.add(s); accepted++; return `${id} (${s.analysis.state})`; }
      open++;
      return id;
    });
    const how = e.how.kind === "purl" ? "purl"
      : e.how.kind === "commit" ? `${e.how.fork ? "fork of" : "commit"} ${e.how.key.slice(0, 7)}`
      : `NVD "${e.how.key}"`;
    lines.push(`${e.c.name} ${e.c.version}`.padEnd(width) + how.padEnd(22) + `${e.ids.length}${verdicts.length ? ": " + verdicts.join(", ") : ""}`);
  }
  // A statement about a scanned component that accepted nothing is stale.
  const scanned = new Set(entries.map((e) => e.c["bom-ref"]));
  for (const s of statements) {
    if (!used.has(s) && s.affects?.some((a) => scanned.has(a.ref))) {
      problems.push(`docs/vex.cdx.json: ${s.id} for ${s.affects.map((a) => a.ref).join(", ")} matches no advisory the scan found; stale`);
    }
  }
  const kinds = { purl: 0, commit: 0, search: 0 };
  for (const e of entries) if (e.how) kinds[e.how.kind]++;
  lines.push(`sbom: ${files.length} document(s), ${entries.length} distinct components, ${kinds.purl} looked up by purl, ${kinds.commit} by upstream commit, ${kinds.search} by NVD search: ${open + accepted} advisories, ${accepted} accepted by docs/vex.cdx.json, ${open} open`);
  if (open) problems.push(`${open} open advisories`);
  return { lines, problems };
}

async function main() {
  const argv = process.argv.slice(2);
  const at = argv.indexOf("--root");
  const proj = project(at >= 0 ? resolve(argv[at + 1]) : ROOT);
  if (at >= 0) argv.splice(at, 2);
  const [cmd, ...args] = argv;
  if (cmd === "check") check(proj);
  else if (cmd === "target" && args.length >= 1) target(proj, args[0], args[1] ? resolve(args[1]) : join(proj.root, "build", "sbom"), args[2]);
  else if (cmd === "scan" && args.length >= 1) {
    const { lines, problems } = await scan(args.map((a) => resolve(a)), { vex: readVex(proj), root: proj.root });
    for (const l of lines) console.log(l);
    for (const p of problems) console.error(`sbom: ${p}`);
    process.exit(problems.length ? 1 : 0);
  } else {
    console.error("usage: sbom.mjs [--root <dir>] check | target <name>|all [outdir] [version] | scan <document>...");
    process.exit(2);
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    await main();
  } catch (e) {
    console.error(`sbom: ${e.message}`);
    process.exit(1);
  }
}
