// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// scripts/sbom.mjs: the hand-kept component list passes its own check, and
// the per-target documents say what each default build of clockwork actually
// contains: the crates cargo resolves for that platform, the web client's
// npm packages, and the vendored code the target's default options compile
// in, each with a version and a licence, and nothing that build does not
// ship (Catch2, Link, the NIF headers, the ASIO SDK are off by default).
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const script = join(root, "scripts", "sbom.mjs");
const TARGETS = ["linux-x64", "linux-arm64", "linux-x86", "macos-arm64", "macos-x64", "windows-x64", "web"];

const out = mkdtempSync(join(tmpdir(), "clockwork-sbom-"));
process.on("exit", () => rmSync(out, { recursive: true, force: true }));

test("the component list passes its own check", () => {
  const text = execFileSync("node", [script, "check"], { encoding: "utf8" });
  assert.match(text, /every vendored path and fetch accounted for/);
});

const docs = {};
test("every target writes a CycloneDX document", () => {
  execFileSync("node", [script, "target", "all", out, "test"], { encoding: "utf8", cwd: root });
  for (const t of TARGETS) {
    const d = JSON.parse(readFileSync(join(out, `clockwork-${t}.cdx.json`), "utf8"));
    assert.equal(d.bomFormat, "CycloneDX");
    assert.equal(d.specVersion, "1.6");
    assert.equal(d.metadata.component.name, "clockwork");
    assert.equal(d.metadata.component.version, "test");
    assert.equal(d.metadata.properties.find((p) => p.name === "clockwork:target").value, t);
    docs[t] = d;
  }
});

const names = (t) => new Set(docs[t].components.map((c) => c.name.split(" ")[0]));

test("every component has a version and a licence, and one entry per name and version", () => {
  for (const t of TARGETS) {
    const seen = new Set();
    for (const c of docs[t].components) {
      assert.ok(c.version, `${t}: ${c.name} has no version`);
      assert.ok(c.licenses?.length, `${t}: ${c.name} has no licence`);
      const key = `${c.name}@${c.version}`;
      assert.ok(!seen.has(key), `${t}: ${key} listed twice`);
      seen.add(key);
    }
  }
});

test("clockwork's own crates are the subject, not components", () => {
  for (const t of TARGETS) {
    for (const c of docs[t].components) assert.ok(!c.name.startsWith("clockwork-"), `${t}: ${c.name}`);
  }
});

test("the vendored code a default build compiles in, and only that", () => {
  for (const t of TARGETS) {
    const n = names(t);
    assert.ok(n.has("oscpack"), `${t}: oscpack`);
    // Off by default, or never shipped.
    for (const off of ["Catch2", "Ableton", "Erlang/OTP", "Steinberg", "libsamplerate"]) assert.ok(!n.has(off), `${t} lists ${off}`);
  }
  for (const t of TARGETS.filter((t) => t !== "web")) {
    const n = names(t);
    for (const native of ["Smoothie", "midir", "CLAP", "VST"]) assert.ok(n.has(native), `${t}: ${native}`);
  }
  const web = names("web");
  for (const notWeb of ["Smoothie", "midir", "CLAP", "VST"]) assert.ok(!web.has(notWeb), `web lists ${notWeb}`);
});

test("the crates follow the platform", () => {
  assert.ok(names("linux-x64").has("alsa"), "linux: alsa");
  assert.ok(names("linux-x64").has("gilrs"), "linux: gilrs");
  assert.ok(!names("macos-arm64").has("gilrs"), "macOS uses the GameController framework, not gilrs");
  assert.ok(names("macos-arm64").has("coremidi"), "macOS: coremidi");
  assert.ok(names("windows-x64").has("windows-sys"), "windows: windows-sys");
  assert.ok(names("windows-x64").has("rusty-xinput"), "windows: xinput");
  assert.ok(!names("linux-x64").has("windows-sys"), "linux without windows-sys");
  assert.ok(names("web").has("wasm-bindgen"), "web: wasm-bindgen");
  assert.ok(!names("web").has("alsa") && !names("web").has("coremidi"), "web without a native MIDI backend");
});

test("the web client's runtime npm packages, and no dev dependency", () => {
  const web = docs.web.components.filter((c) => c.purl?.startsWith("pkg:npm/"));
  assert.ok(web.some((c) => c.name === "@thi.ng/malloc"), "@thi.ng/malloc");
  assert.ok(web.every((c) => c.name !== "esbuild" && c.name !== "@playwright/test"), "no dev dependency");
  assert.ok(web.every((c) => c.purl.startsWith("pkg:npm/%40thi.ng/")), "purls name the scope");
  for (const t of TARGETS.filter((t) => t !== "web")) {
    assert.ok(!docs[t].components.some((c) => c.purl?.startsWith("pkg:npm/")), `${t} has no npm packages`);
  }
});

// The scan, with the network stood in for: every component of every document
// is looked up one way or another, nothing vendored is skipped, an advisory
// fails the scan unless a VEX statement accepts it, a stale statement fails
// it, and a fetched library's recorded commit must still be its tag.
const { lookup, scan } = await import(script);
const files = TARGETS.map((t) => join(out, `clockwork-${t}.cdx.json`));
const canary = join(root, "test", "fixtures", "sbom-canary.cdx.json");
const quiet = { osv: async (qs) => qs.map(() => []), nvd: async () => [], tag: () => undefined, vex: { vulnerabilities: [] } };
// Answers as the list records them, so the tag check passes.
const list = JSON.parse(readFileSync(join(root, "docs", "components.cdx.json"), "utf8"));
const recorded = (url, tag) => list.components.find((c) => c.version === tag && c.externalReferences?.some((r) => r.url === url))
  ?.properties.find((p) => p.name === "clockwork:upstream_commit")?.value;

test("every component of every document can be looked up, vendored code by its upstream commit", () => {
  for (const t of TARGETS) {
    for (const c of docs[t].components) {
      const how = lookup(c);
      assert.ok(how, `${t}: ${c.name} ${c.version} cannot be looked up`);
      if (!c.purl?.startsWith("pkg:cargo/") && !c.purl?.startsWith("pkg:npm/")) {
        assert.equal(how.kind, "commit", `${t}: ${c.name} is vendored and must be looked up by commit`);
        assert.match(how.key, /^[0-9a-f]{40}$/);
      }
    }
  }
  assert.equal(lookup(JSON.parse(readFileSync(canary, "utf8")).components[2]).kind, "search", "a keyword search when there is no repository");
  assert.equal(lookup({ name: "x", version: "1" }), null);
});

test("a clean scan: every component queried, forks said to be forks, no open advisory", async () => {
  const queried = [];
  const { lines, problems } = await scan(files, { ...quiet, osv: async (qs) => { queried.push(...qs); return qs.map(() => []); }, tag: recorded });
  assert.deepEqual(problems, []);
  const distinct = new Set(TARGETS.flatMap((t) => docs[t].components.map((c) => `${c.name}@${c.version}`)));
  assert.equal(queried.length, distinct.size, "one query per distinct component");
  assert.ok(queried.some((q) => q.commit === "4f43011b96eb0636104cb3e433894cda98243626"), "JUCE's fork point is queried");
  assert.ok(lines.some((l) => l.startsWith("Smoothie") && l.includes("fork of 4f43011")), "Smoothie is reported as a fork of JUCE");
  assert.ok(lines.some((l) => l.startsWith("CLAP") && l.includes("commit 195b42a")), "an unmodified copy is reported by its commit");
  assert.match(lines.at(-1), /0 advisories, 0 accepted by docs\/vex.cdx.json, 0 open/);
});

test("an advisory fails the scan, and a VEX statement for that component accepts it", async () => {
  const osv = async (qs) => qs.map((q) => (q.commit === "77ef129cf58123a4fe9cf0f8f928fbd9515478b7" ? ["OSV-2020-1372"] : []));
  const open = await scan(files, { ...quiet, osv, tag: recorded });
  assert.deepEqual(open.problems, ["1 open advisories"]);
  assert.ok(open.lines.some((l) => l.startsWith("oscpack 1.1.0") && l.endsWith("1: OSV-2020-1372")));

  const statement = { id: "OSV-2020-1372", affects: [{ ref: "oscpack" }], analysis: { state: "not_affected", justification: "code_not_present" } };
  const accepted = await scan(files, { ...quiet, osv, tag: recorded, vex: { vulnerabilities: [statement] } });
  assert.deepEqual(accepted.problems, []);
  assert.ok(accepted.lines.some((l) => l.startsWith("oscpack 1.1.0") && l.endsWith("1: OSV-2020-1372 (not_affected)")));

  // A statement for another component, or one under investigation, accepts nothing.
  for (const wrong of [{ ...statement, affects: [{ ref: "clap" }] }, { ...statement, analysis: { state: "in_triage" } }]) {
    const r = await scan(files, { ...quiet, osv, tag: recorded, vex: { vulnerabilities: [wrong] } });
    assert.ok(r.problems.includes("1 open advisories"), JSON.stringify(wrong));
  }
});

test("a VEX statement that matches nothing is stale and fails the scan", async () => {
  const stale = { id: "CVE-2000-0001", affects: [{ ref: "oscpack" }], analysis: { state: "resolved" } };
  const { problems } = await scan(files, { ...quiet, tag: recorded, vex: { vulnerabilities: [stale] } });
  assert.equal(problems.length, 1);
  assert.match(problems[0], /CVE-2000-0001 for oscpack matches no advisory the scan found; stale/);
});

test("a fetched library whose tag no longer points at the recorded commit fails the scan", async () => {
  const moved = (url, tag) => (tag === "1.2.10" ? "0000000000000000000000000000000000000000" : recorded(url, tag));
  const { problems } = await scan(files, { ...quiet, tag: moved });
  assert.equal(problems.length, 1);
  assert.match(problems[0], /CLAP: 1\.2\.10 is 0{40} at .* the list records 195b42a/);
});

test("a component that cannot be looked up fails the scan rather than being skipped", async () => {
  const file = join(out, "unsourced.cdx.json");
  writeFileSync(file, JSON.stringify({ bomFormat: "CycloneDX", components: [{ "bom-ref": "x", name: "mystery", version: "1.0", properties: [{ name: "clockwork:modified", value: "no" }] }] }));
  const { problems } = await scan([file], quiet);
  assert.deepEqual(problems, ["mystery 1.0: no purl, clockwork:upstream_commit or clockwork:advisory_search; not looked up"]);
});

test("the canary fixture is looked up each of the three ways and fails on what it finds", async () => {
  const osv = async (qs) => qs.map((q) => (q.commit ? ["CVE-2022-37434"] : ["GHSA-p6mc-m468-83gw"]));
  const nvd = async (keyword) => (keyword === "zlib inflate" ? ["CVE-2022-37434"] : []);
  const { lines, problems } = await scan([canary], { ...quiet, osv, nvd });
  assert.deepEqual(problems, ["3 open advisories"]);
  assert.match(lines.at(-1), /1 looked up by purl, 1 by upstream commit, 1 by NVD search: 3 advisories/);
});

// A project that pins clockwork (SuperSonic's shape): its own list and
// settings, a fork that carries another project's code inside it, and
// clockwork at the pin. Run with --root.
const { project, readVex } = await import(script);
const fork = "abcdef1" + "0".repeat(33);
const lib = "1234567" + "0".repeat(33);
function fixtureProject(edit = (l) => l, dir = mkdtempSync(join(tmpdir(), "clockwork-sbom-project-"))) {
  process.on("exit", () => rmSync(dir, { recursive: true, force: true }));
  mkdirSync(join(dir, "docs"), { recursive: true });
  mkdirSync(join(dir, "fork", "vendor", "lib"), { recursive: true });
  writeFileSync(join(dir, "fork", "VERSION.h"), '#define FORK_COMMIT "abcdef1"\n#define V_MAJOR 1\n#define V_MINOR 2\n#define V_TWEAK "-dev"\n');
  writeFileSync(join(dir, "CMakeLists.txt"), 'option(PRJ_THING "a thing" ON)\n');
  symlinkSync(root, join(dir, "clockwork"));
  const p = (name, value) => ({ name: `clockwork:${name}`, value });
  const l = {
    bomFormat: "CycloneDX", specVersion: "1.6", version: 1,
    metadata: {
      component: { type: "library", "bom-ref": "fixture", name: "fixture" },
      properties: [p("vendor-dirs", "fork/vendor"), p("option-prefix", "PRJ_"), p("pins", "clockwork")],
    },
    components: [
      { type: "library", "bom-ref": "fork", name: "fork", version: "1.2-dev", properties: [
        p("path", "fork"), p("targets", "native,web"), p("version_file", "fork/VERSION.h"),
        p("version_pattern", 'V_MAJOR (\\d+)[\\s\\S]*?V_MINOR (\\d+)[\\s\\S]*?V_TWEAK "([^"]*)"'),
        p("upstream_commit", fork), p("commit_file", "fork/VERSION.h"), p("commit_pattern", 'FORK_COMMIT "([0-9a-f]+)"'), p("modified", "yes")] },
      { type: "library", "bom-ref": "lib", name: "lib", version: "1234567", properties: [
        p("path", "fork/vendor/lib"), p("targets", "native,web"), p("version_from", "upstream_commit"), p("upstream_commit", lib)] },
    ],
  };
  writeFileSync(join(dir, "docs", "components.cdx.json"), JSON.stringify(edit(l), null, 2));
  return dir;
}
const checkProject = (dir) => {
  try {
    return { ok: true, text: execFileSync("node", [script, "--root", dir, "check"], { encoding: "utf8", stdio: "pipe" }) };
  } catch (e) {
    return { ok: false, text: e.stderr.toString() };
  }
};
const prop = (c, n) => c.properties?.find((x) => x.name === `clockwork:${n}`)?.value;

test("a project that pins clockwork passes its check and refers to clockwork's document for each target", () => {
  const dir = fixtureProject();
  assert.ok(checkProject(dir).ok);
  const outdir = join(dir, "sbom");
  execFileSync("node", [script, "--root", dir, "target", "web", outdir, "v1"], { encoding: "utf8" });
  const own = JSON.parse(readFileSync(join(outdir, "fixture-web.cdx.json"), "utf8"));
  const cw = JSON.parse(readFileSync(join(outdir, "clockwork-web.cdx.json"), "utf8"));
  const head = execFileSync("git", ["-C", root, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
  assert.equal(cw.metadata.component.version, head.slice(0, 7));
  const pinned = own.components.find((c) => c.name === "clockwork");
  assert.equal(pinned.version, head.slice(0, 7));
  assert.equal(prop(pinned, "upstream_commit"), head);
  const link = pinned.externalReferences.find((r) => r.type === "bom");
  assert.equal(link.url, `urn:cdx:${cw.serialNumber.replace(/^urn:uuid:/, "")}/1`);
  assert.deepEqual(own.components.map((c) => c.name).sort(), ["clockwork", "fork", "lib"]);
});

test("a fork's version is read with its pre-release suffix, and its upstream commit is held to the one its source records", () => {
  const v = checkProject(fixtureProject((l) => { l.components[0].version = "1.2"; return l; }));
  assert.ok(!v.ok);
  assert.match(v.text, /fork: fork\/VERSION.h says 1.2-dev, the list says 1.2/);
  const r = checkProject(fixtureProject((l) => {
    l.components[0].properties.find((x) => x.name === "clockwork:upstream_commit").value = "f".repeat(40);
    return l;
  }));
  assert.ok(!r.ok);
  assert.match(r.text, /fork: fork\/VERSION.h records upstream commit abcdef1, the list records f{40}/);
});

test("code with no release versions is versioned by the first seven characters of its upstream commit", () => {
  const r = checkProject(fixtureProject((l) => { l.components[1].version = "7654321"; return l; }));
  assert.ok(!r.ok);
  assert.match(r.text, /lib: versioned by its upstream commit, but 7654321 is not/);
});

test("code vendored inside a fork needs an entry of its own", () => {
  const r = checkProject(fixtureProject((l) => { l.components.splice(1, 1); return l; }));
  assert.ok(!r.ok);
  assert.match(r.text, /fork\/vendor\/lib: vendored, but not in docs\/components.cdx.json/);
});

// Code we forked and maintain as our own is listed as an ancestor in the
// subject's pedigree, not as a component.
const asAncestor = (l) => {
  const [fork] = l.components.splice(0, 1);
  l.metadata.component.pedigree = { ancestors: [fork] };
  return l;
};

test("an ancestor is checked against its upstream commit, appears in each document's metadata and is not scanned", async () => {
  const dir = fixtureProject(asAncestor);
  assert.ok(checkProject(dir).ok);
  const moved = checkProject(fixtureProject((l) => {
    asAncestor(l).metadata.component.pedigree.ancestors[0].properties.find((x) => x.name === "clockwork:upstream_commit").value = "f".repeat(40);
    return l;
  }));
  assert.ok(!moved.ok);
  assert.match(moved.text, /fork: fork\/VERSION.h records upstream commit abcdef1, the list records f{40}/);

  const outdir = join(dir, "sbom");
  execFileSync("node", [script, "--root", dir, "target", "web", outdir, "v1"], { encoding: "utf8" });
  const file = join(outdir, "fixture-web.cdx.json");
  const own = JSON.parse(readFileSync(file, "utf8"));
  assert.deepEqual(own.metadata.component.pedigree.ancestors.map((a) => a.name), ["fork"]);
  assert.deepEqual(own.components.map((c) => c.name).sort(), ["clockwork", "lib"]);

  const queried = [];
  const osv = async (qs) => { queried.push(...qs); return qs.map(() => []); };
  await scan([file], { ...quiet, osv, tag: recorded, vex: { vulnerabilities: [] } });
  assert.ok(queried.length > 0);
  assert.ok(!queried.some((q) => q.commit === fork), "the ancestor is not looked up");
});

test("a project's scan applies clockwork's VEX statements from the pin as well as its own", () => {
  const dir = fixtureProject();
  writeFileSync(join(dir, "docs", "vex.cdx.json"), JSON.stringify({ vulnerabilities: [{ id: "TEST-1", affects: [{ ref: "fork" }], analysis: { state: "not_affected" } }] }));
  const ids = readVex(project(dir)).vulnerabilities.map((v) => v.id);
  const clockworks = JSON.parse(readFileSync(join(root, "docs", "vex.cdx.json"), "utf8")).vulnerabilities.map((v) => v.id);
  assert.deepEqual(ids, ["TEST-1", ...clockworks]);
});

// A chain of pins (Sonic Pi's shape): a top project pins a middle one, which
// pins clockwork. Each is a git checkout, as a submodule is.
const git = (dir, ...args) => execFileSync("git", ["-C", dir, "-c", "user.name=t", "-c", "user.email=t@t", ...args], { encoding: "utf8", stdio: "pipe" });
function commitAll(dir) {
  git(dir, "init", "-q");
  git(dir, "add", "-A");
  git(dir, "commit", "-q", "-m", "fixture");
  return git(dir, "rev-parse", "HEAD").trim();
}
function bareProject(dir, name, pins, extra = {}) {
  mkdirSync(join(dir, "docs"), { recursive: true });
  const p = (n, value) => ({ name: `clockwork:${n}`, value });
  writeFileSync(join(dir, "docs", "components.cdx.json"), JSON.stringify({
    bomFormat: "CycloneDX", specVersion: "1.6", version: 1,
    metadata: { component: { type: "application", "bom-ref": name, name }, properties: [p("vendor-dirs", ""), p("pins", pins)] },
    components: [],
  }));
  if (extra.vex) writeFileSync(join(dir, "docs", "vex.cdx.json"), JSON.stringify({ vulnerabilities: [extra.vex] }));
}
function chain() {
  const top = mkdtempSync(join(tmpdir(), "clockwork-sbom-chain-"));
  process.on("exit", () => rmSync(top, { recursive: true, force: true }));
  const mid = join(top, "mid");
  fixtureProject((l) => l, mid);
  const midCommit = commitAll(mid);
  return { top, mid, midCommit };
}
const linkTo = (component, doc) => component.externalReferences.find((r) => r.type === "bom").url === `urn:cdx:${doc.serialNumber.replace(/^urn:uuid:/, "")}/1`;

test("each project in a chain of pins lists its direct pins, linked to their documents", () => {
  const { top, midCommit } = chain();
  bareProject(top, "top", "mid");
  const outdir = join(top, "sbom");
  execFileSync("node", [script, "--root", top, "target", "web", outdir, "v1"], { encoding: "utf8" });
  const read = (n) => JSON.parse(readFileSync(join(outdir, `${n}-web.cdx.json`), "utf8"));
  const [own, mid, cw] = [read("top"), read("fixture"), read("clockwork")];
  assert.deepEqual(own.components.map((c) => c.name), ["fixture"]);
  assert.equal(own.components[0].version, midCommit.slice(0, 7));
  assert.ok(linkTo(own.components[0], mid));
  const pinned = mid.components.find((c) => c.name === "clockwork");
  assert.ok(linkTo(pinned, cw));
  assert.ok(!own.components.some((c) => c.name === "clockwork"), "a pin's pins are reached through it, not listed again");
});

test("a project pinned twice at one commit is written once and linked from both", () => {
  const { top } = chain();
  bareProject(top, "top", "mid,mid/clockwork");
  const outdir = join(top, "sbom");
  const text = execFileSync("node", [script, "--root", top, "target", "web", outdir, "v1"], { encoding: "utf8" });
  assert.equal(text.match(/clockwork-web\.cdx\.json/g).length, 1);
  const own = JSON.parse(readFileSync(join(outdir, "top-web.cdx.json"), "utf8"));
  const cw = JSON.parse(readFileSync(join(outdir, "clockwork-web.cdx.json"), "utf8"));
  const mid = JSON.parse(readFileSync(join(outdir, "fixture-web.cdx.json"), "utf8"));
  assert.ok(linkTo(own.components.find((c) => c.name === "clockwork"), cw));
  assert.ok(linkTo(mid.components.find((c) => c.name === "clockwork"), cw));
});

test("a project pinned at two commits stops the run", () => {
  const { top } = chain();
  const other = join(top, "other");
  bareProject(other, "clockwork", "");
  commitAll(other);
  bareProject(top, "top", "mid,other");
  let err = "";
  try {
    execFileSync("node", [script, "--root", top, "target", "web", join(top, "sbom"), "v1"], { encoding: "utf8", stdio: "pipe" });
  } catch (e) {
    err = e.stderr.toString();
  }
  assert.match(err, /clockwork is pinned at [0-9a-f]{7} and at [0-9a-f]{7} \(other\)/);
});

test("the VEX statements of every project in the chain apply, each tree once", () => {
  const { top } = chain();
  bareProject(top, "top", "mid,mid/clockwork", { vex: { id: "TOP-1", affects: [{ ref: "x" }], analysis: { state: "not_affected" } } });
  writeFileSync(join(top, "mid", "docs", "vex.cdx.json"), JSON.stringify({ vulnerabilities: [{ id: "MID-1", affects: [{ ref: "fork" }], analysis: { state: "not_affected" } }] }));
  const ids = readVex(project(top)).vulnerabilities.map((v) => v.id);
  const clockworks = JSON.parse(readFileSync(join(root, "docs", "vex.cdx.json"), "utf8")).vulnerabilities.map((v) => v.id);
  assert.deepEqual(ids, ["TOP-1", "MID-1", ...clockworks]);
});
