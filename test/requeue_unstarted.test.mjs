// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// .github/actions/requeue-unstarted: a job GitHub cancelled before it ran a
// single step is one no runner took, not a verdict on the code. When such a
// job fails a run, the action queues it again, three times at most. A run
// that a person cancelled, or that a newer push superseded, is left alone,
// and so is a run with a job in it that ran and failed.
import { test } from "node:test";
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { decide, parseJobs, requests } from "../.github/actions/requeue-unstarted/requeue.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const script = join(root, ".github", "actions", "requeue-unstarted", "requeue.mjs");

const run = (over = {}) => ({ id: 42, status: "completed", conclusion: "failure", run_attempt: 1, ...over });
const ran = (name, conclusion = "success") => ({ id: name.length, name, status: "completed", conclusion, steps: 9 });
const unstarted = (name) => ({ id: name.length * 100, name, status: "completed", conclusion: "cancelled", steps: 0 });

test("a failed run's jobs that no runner took are queued again", () => {
  const d = decide(run(), [ran("linux"), ran("docs", "skipped"), unstarted("nif-macos-arm64"), unstarted("sanitize-tsan-macos-arm64")]);
  assert.equal(d.action, "requeue");
  assert.deepEqual(d.jobs.map((j) => j.name), ["nif-macos-arm64", "sanitize-tsan-macos-arm64"]);
});

// GitHub refuses a second request about a run the first has set going again
// ("The workflow run containing this job is already running"), so however
// many jobs went unstarted, the run is asked once, for all of them.
test("the unstarted jobs are asked for in one request for the run", () => {
  const d = decide(run(), [ran("linux"), unstarted("a"), unstarted("b"), unstarted("c"), unstarted("d")]);
  assert.deepEqual(requests("o/r", 42, d), [["api", "-X", "POST", "repos/o/r/actions/runs/42/rerun-failed-jobs"]]);
});

test("nothing is asked of a run that is left alone", () => {
  assert.deepEqual(requests("o/r", 42, decide(run({ conclusion: "success" }), [ran("linux")])), []);
  assert.deepEqual(requests("o/r", 42, decide(run({ run_attempt: 4 }), [unstarted("nif")])), []);
});

// that one request runs again every job that is not green, so a run with a
// job that ran and failed is red whatever the unstarted ones would say
test("a run with a job that ran and failed is left alone, its unstarted jobs too", () => {
  const d = decide(run(), [ran("linux"), ran("windows", "failure"), unstarted("nif-macos-arm64")]);
  assert.equal(d.action, "none");
  assert.match(d.reason, /ran and did not pass: windows/);
});

test("a run still going is left to finish", () => {
  const d = decide(run({ status: "in_progress", conclusion: null }), [unstarted("nif-macos-arm64")]);
  assert.equal(d.action, "none");
  assert.match(d.reason, /still running/);
});

test("a green run has nothing to queue", () => {
  const d = decide(run({ conclusion: "success" }), [ran("linux")]);
  assert.equal(d.action, "none");
});

test("a cancelled run was stopped on purpose, or a newer push took over: its unstarted jobs stay cancelled", () => {
  const d = decide(run({ conclusion: "cancelled" }), [ran("linux", "cancelled"), unstarted("nif-macos-arm64")]);
  assert.equal(d.action, "none");
  assert.match(d.reason, /cancelled/);
});

test("a failed run whose every job ran is a real failure", () => {
  const d = decide(run(), [ran("linux"), ran("windows", "failure")]);
  assert.equal(d.action, "none");
  assert.match(d.reason, /every job ran/);
});

test("a job cancelled after it started ran, and is not queued again", () => {
  const started = { id: 1, name: "linux", status: "completed", conclusion: "cancelled", steps: 3 };
  assert.equal(decide(run(), [started]).action, "none");
  assert.equal(decide(run(), [started, unstarted("nif-macos-arm64")]).action, "none");
});

test("three times, then give up", () => {
  assert.equal(decide(run({ run_attempt: 3 }), [unstarted("nif")]).action, "requeue");
  const d = decide(run({ run_attempt: 4 }), [unstarted("nif")]);
  assert.equal(d.action, "give-up");
  assert.match(d.reason, /three times/);
});

test("the jobs come from gh's paginated output, one object per line", () => {
  const lines = '{"id":1,"name":"a","status":"completed","conclusion":"success","steps":[{"n":1},{"n":2}]}\n'
              + '{"id":2,"name":"b","status":"completed","conclusion":"cancelled","steps":[]}\n';
  assert.deepEqual(parseJobs(lines), [
    { id: 1, name: "a", status: "completed", conclusion: "success", steps: 2 },
    { id: 2, name: "b", status: "completed", conclusion: "cancelled", steps: 0 },
  ]);
});

test("the command refuses to run without a run id", () => {
  assert.throws(() => execFileSync("node", [script, "--repo", "o/r"], { encoding: "utf8", stdio: "pipe" }),
                /--run/);
});
