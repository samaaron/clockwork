#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
"""Judge a CodeQL analysis by Clockwork's own code, in two steps around the upload.

    codeql-report.py trim IN.sarif OUT.sarif
    codeql-report.py judge

TRIM runs before the upload. The analysis compiles everything, so its results
cover Smoothie (our JUCE fork), the vendored single-file libraries, the plugin
SDKs and the fetched dependencies as well as Clockwork. Those findings are
their authors' and would bury ours (345 of the first run's 443), and CodeQL's
paths-ignore does not apply to a compiled language built by hand, so the
results are trimmed here instead: OUT.sarif keeps only the findings in
Clockwork's own sources, and is what gets uploaded, so the Security tab shows
the same set. The findings go to the log and the job summary. Exit 2 when the
file does not prove an analysis happened: no results at all, or a rule whose
severity cannot be found, is not a clean bill of health.

JUDGE runs after the upload has been processed, and asks GitHub rather than
the file: every alert still OPEN on this ref at warning or error level fails
the job, the same bar as a compiler warning under -Werror. Asking GitHub is
what lets a finding that is there by design be dismissed once, in the Security
tab with its reason, instead of being argued with on every push. Needs
GITHUB_TOKEN, GITHUB_REPOSITORY and GITHUB_REF; exit 2 without them, or when
the listing cannot be read.
"""
import json
import os
import sys
import urllib.parse
import urllib.request

THIRD_PARTY = ("smoothie/", "src/vendor/", "plugins/", "build/")
FAILING = ("error", "warning")
ORDER = {"error": 0, "?": 1, "warning": 2, "note": 3}


def report(title, lines):
    text = "\n".join(lines)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as s:
            s.write(f"## {title}\n\n{text}\n\n")


def table(rows):
    out = ["", "| level | rule | where | message |", "|---|---|---|---|"]
    out += [f"| {l} | {r} | {w} | {m.replace('|', '/')} |" for l, r, w, m in rows]
    return out


def severities(run):
    """Rule id -> level. CodeQL files its rules under the query-pack extensions;
    the upload API's copy moves them to the driver, so both are read."""
    out = {}
    tool = run.get("tool", {})
    for component in [tool.get("driver", {})] + tool.get("extensions", []):
        for rule in component.get("rules", []):
            out[rule["id"]] = rule.get("defaultConfiguration", {}).get("level", "warning")
    return out


def trim(src, dst):
    sarif = json.load(open(src))
    rows, dropped, unknown = [], 0, []
    for run in sarif.get("runs", []):
        levels = severities(run)
        kept = []
        for r in run.get("results", []):
            loc = r.get("locations", [{}])[0].get("physicalLocation", {})
            path = loc.get("artifactLocation", {}).get("uri", "?")
            if path.startswith(THIRD_PARTY):
                dropped += 1
                continue
            kept.append(r)
            level = r.get("level") or levels.get(r.get("ruleId"))
            if level is None:
                unknown.append(r.get("ruleId"))
                level = "?"
            line = loc.get("region", {}).get("startLine", 0)
            rows.append((level, r.get("ruleId"), f"{path}:{line}",
                         r.get("message", {}).get("text", "").split("\n")[0][:120]))
        run["results"] = kept
    json.dump(sarif, open(dst, "w"))

    rows.sort(key=lambda x: (ORDER.get(x[0], 9), x[1], x[2]))
    seen = rows or dropped
    lines = [f"CodeQL found {len(rows)} findings in Clockwork's code "
             f"({dropped} third-party findings set aside)"]
    if rows:
        lines += table(rows)
    if not seen:
        lines.append("no results in the SARIF at all: the analysis did not look")
    if unknown:
        lines.append(f"{len(unknown)} findings with no severity on record: "
                     + ", ".join(sorted(set(unknown))))
    report("CodeQL (c-cpp): the analysis", lines)
    return 2 if (not seen or unknown) else 0


def alerts(repo, token, ref, state):
    """Every alert in one state on one ref, through the paged listing."""
    out, page = [], 1
    while True:
        q = urllib.parse.urlencode({"ref": ref, "state": state, "per_page": 100, "page": page})
        req = urllib.request.Request(
            f"https://api.github.com/repos/{repo}/code-scanning/alerts?{q}",
            headers={"Authorization": f"Bearer {token}",
                     "Accept": "application/vnd.github+json",
                     "X-GitHub-Api-Version": "2022-11-28"})
        with urllib.request.urlopen(req) as resp:
            batch = json.load(resp)
        out += batch
        if len(batch) < 100:
            return out
        page += 1


def judge():
    repo, token, ref = (os.environ.get(k) for k in ("GITHUB_REPOSITORY", "GITHUB_TOKEN", "GITHUB_REF"))
    if not (repo and token and ref):
        report("CodeQL (c-cpp): the verdict",
               ["cannot ask GitHub for the alerts: GITHUB_REPOSITORY, GITHUB_TOKEN and GITHUB_REF are needed"])
        return 2
    try:
        open_alerts = alerts(repo, token, ref, "open")
        dismissed = alerts(repo, token, ref, "dismissed")
    except OSError as e:
        report("CodeQL (c-cpp): the verdict", [f"cannot read the alerts for {ref}: {e}"])
        return 2

    def row(a):
        loc = a["most_recent_instance"]["location"]
        return (a["rule"]["severity"], a["rule"]["id"],
                f"{loc['path']}:{loc['start_line']}",
                a["most_recent_instance"]["message"]["text"].split("\n")[0][:120])

    ours = [a for a in open_alerts
            if not a["most_recent_instance"]["location"]["path"].startswith(THIRD_PARTY)]
    failing = sorted((row(a) for a in ours if a["rule"]["severity"] in FAILING),
                     key=lambda x: (ORDER.get(x[0], 9), x[1], x[2]))
    notes = len(ours) - len(failing)
    lines = [f"{len(ours)} alerts open on {ref} in Clockwork's code: {len(failing)} at warning or error, "
             f"{notes} notes; {len(dismissed)} dismissed as by design or false"]
    if failing:
        lines += table(failing)
        lines += ["", "fix each, or dismiss it in the Security tab with the reason it stands"]
    report("CodeQL (c-cpp): the verdict", lines)
    return 1 if failing else 0


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "trim":
        sys.exit(trim(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 2 and sys.argv[1] == "judge":
        sys.exit(judge())
    sys.exit(__doc__)
