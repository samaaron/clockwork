#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// Queue again the jobs of a failed workflow run that no runner ever took.
//
// GitHub sometimes cancels a job before it runs a single step ("The job was
// not acquired by Runner of type hosted"): the pool had no runner for it.
// The run then shows as failed, and nothing about the code was tested or
// found wanting. This asks for those jobs again, three times at most, and
// leaves every other kind of red alone: a job that ran and failed, a run a
// person cancelled, a run a newer push superseded (those are cancelled, not
// failed).
//
//   node requeue.mjs --repo owner/name --run <id> [--max-attempts 3] [--dry-run]
//
// Talks to GitHub through gh, with the token gh finds (GH_TOKEN on a
// runner). The caller needs actions: write to queue a job again.
import { execFileSync } from "node:child_process";

export const MAX_ATTEMPTS = 3;

// What to do about a run, from the run itself and its jobs (parseJobs'
// shape): { action: "none" | "requeue" | "give-up", reason, jobs }.
export function decide(run, jobs, { maxAttempts = MAX_ATTEMPTS } = {}) {
  if (run.status !== "completed") return { action: "none", reason: "still running", jobs: [] };
  if (run.conclusion !== "failure") return { action: "none", reason: `not a failure: ${run.conclusion}`, jobs: [] };
  const unstarted = jobs.filter((j) => j.conclusion === "cancelled" && j.steps === 0);
  if (unstarted.length === 0) return { action: "none", reason: "every job ran: a real failure", jobs: [] };
  if (run.run_attempt > maxAttempts) {
    return { action: "give-up", reason: `no runner took them three times (attempt ${run.run_attempt}): giving up`, jobs: unstarted };
  }
  return { action: "requeue", reason: `no runner took ${unstarted.length} job(s) on attempt ${run.run_attempt}`, jobs: unstarted };
}

// gh api --paginate prints one JSON object per line; a job's steps are
// counted, nothing else about them matters here.
export function parseJobs(text) {
  return text.split("\n").filter((l) => l.trim()).map((l) => {
    const j = JSON.parse(l);
    return { id: j.id, name: j.name, status: j.status, conclusion: j.conclusion, steps: j.steps.length };
  });
}

function gh(args) {
  return execFileSync("gh", args, { encoding: "utf8", stdio: ["ignore", "pipe", "inherit"] });
}

function main(argv) {
  const opt = (name, fallback) => {
    const i = argv.indexOf(name);
    return i >= 0 ? argv[i + 1] : fallback;
  };
  const repo = opt("--repo", process.env.GITHUB_REPOSITORY);
  const runId = opt("--run");
  const maxAttempts = Number(opt("--max-attempts", MAX_ATTEMPTS));
  const dryRun = argv.includes("--dry-run");
  if (!repo) throw new Error("--repo owner/name (or GITHUB_REPOSITORY) is needed");
  if (!runId) throw new Error("--run <workflow run id> is needed");

  const base = `repos/${repo}/actions/runs/${runId}`;
  const run = JSON.parse(gh(["api", base]));
  const jobs = parseJobs(gh(["api", `${base}/jobs?per_page=100`, "--paginate", "--jq", ".jobs[]"]));
  const d = decide(run, jobs, { maxAttempts });
  console.log(`${run.name ?? "run"} ${runId} (attempt ${run.run_attempt}, ${run.status}${run.conclusion ? `, ${run.conclusion}` : ""}): ${d.reason}`);
  for (const j of d.jobs) console.log(`  ${j.name}: no runner took it`);
  if (d.action !== "requeue") return;
  for (const j of d.jobs) {
    if (dryRun) { console.log(`  would queue ${j.name} again`); continue; }
    gh(["api", "-X", "POST", `repos/${repo}/actions/jobs/${j.id}/rerun`]);
    console.log(`  queued ${j.name} again`);
  }
}

if (process.argv[1] === new URL(import.meta.url).pathname) main(process.argv.slice(2));
