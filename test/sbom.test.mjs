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
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
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
