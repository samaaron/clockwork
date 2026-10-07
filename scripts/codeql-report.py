#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
"""Judge a CodeQL analysis by Clockwork's own code, in two steps around the upload.

    codeql-report.py trim IN.sarif OURS.sarif [SMOOTHIE.sarif]
    codeql-report.py judge

TRIM runs before the upload. A C++ analysis compiles everything, so its
results cover Smoothie (our JUCE fork), the vendored single-file libraries,
the plugin SDKs and the fetched dependencies as well as Clockwork. Those
findings are their authors' and would bury ours (345 of the first run's 443),
and CodeQL's paths-ignore does not apply to a compiled language built by
hand, so the results are split here instead: OURS.sarif keeps the findings in
Clockwork's own sources and is what the gate is judged by; SMOOTHIE.sarif,
when asked for, keeps Smoothie's, which are ours to work down in their own
time and are uploaded under a category of their own. The rest are set aside.
The findings go to the log and the job summary. Exit 2 when the file does not
prove an analysis happened: no results at all, or a rule whose severity
cannot be found, is not a clean bill of health.

JUDGE runs after the upload has been processed, and asks GitHub rather than
the file: every alert still OPEN on this ref at warning or error level in
Clockwork's code fails the job, the same bar as a compiler warning under
-Werror, whichever language or platform found it. Smoothie's alerts are
counted and shown, not judged. Asking GitHub is what lets a finding that is
there by design be dismissed once, in the Security tab with its reason,
instead of being argued with on every push. Needs GITHUB_TOKEN,
GITHUB_REPOSITORY and GITHUB_REF; exit 2 without them, or when the listing
cannot be read.
"""
import json
import os
import sys
import urllib.parse
import urllib.request

SMOOTHIE = ("smoothie/",)
SET_ASIDE = ("src/vendor/", "plugins/", "build/", "node_modules/", "rust/target/")

# A rule that cannot judge the code it is run on, by rule id and the path it
# does not suit, with the check that does the job instead:
#   rust/access-invalid-pointer over rust/: the crates are an allocator and
#   ring readers over the shared arena, where every pointer is an offset the
#   arena published and the rule can prove none of them, so it flags each
#   dereference in each SAFETY block. Their validity is executed, not
#   inferred: the invariant checker in clockwork-heap and the Miri job in CI.
UNSUITED = (("rust/access-invalid-pointer", "rust/"),)


def unsuited(rule, path):
    return any(rule == r and path.startswith(p) for r, p in UNSUITED)
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


def location(result):
    loc = result.get("locations", [{}])[0].get("physicalLocation", {})
    return (loc.get("artifactLocation", {}).get("uri", "?"),
            loc.get("region", {}).get("startLine", 0))


def with_results(sarif, keep):
    """A copy of the SARIF holding only the results `keep(rule, path)` says to."""
    out = json.loads(json.dumps(sarif))
    for run in out.get("runs", []):
        run["results"] = [r for r in run.get("results", []) if keep(r.get("ruleId"), location(r)[0])]
    return out


def trim(src, ours_path, smoothie_path=None):
    sarif = json.load(open(src))
    rows, smoothie, aside, unknown = [], 0, 0, []
    for run in sarif.get("runs", []):
        levels = severities(run)
        for r in run.get("results", []):
            path, line = location(r)
            if path.startswith(SMOOTHIE):
                smoothie += 1
                continue
            if path.startswith(SET_ASIDE) or unsuited(r.get("ruleId"), path):
                aside += 1
                continue
            level = r.get("level") or levels.get(r.get("ruleId"))
            if level is None:
                unknown.append(r.get("ruleId"))
                level = "?"
            rows.append((level, r.get("ruleId"), f"{path}:{line}",
                         r.get("message", {}).get("text", "").split("\n")[0][:120]))
    json.dump(with_results(sarif, lambda rule, p: not p.startswith(SMOOTHIE + SET_ASIDE) and not unsuited(rule, p)),
              open(ours_path, "w"))
    if smoothie_path:
        json.dump(with_results(sarif, lambda rule, p: p.startswith(SMOOTHIE)), open(smoothie_path, "w"))

    rows.sort(key=lambda x: (ORDER.get(x[0], 9), x[1], x[2]))
    seen = rows or smoothie or aside
    lines = [f"CodeQL found {len(rows)} findings in Clockwork's code, {smoothie} in Smoothie "
             f"({aside} third-party findings set aside)"]
    if rows:
        lines += table(rows)
    if not seen:
        lines.append("no results in the SARIF at all: the analysis did not look")
    if unknown:
        lines.append(f"{len(unknown)} findings with no severity on record: "
                     + ", ".join(sorted(set(unknown))))
    report("CodeQL: the analysis", lines)
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
        report("CodeQL: the verdict",
               ["cannot ask GitHub for the alerts: GITHUB_REPOSITORY, GITHUB_TOKEN and GITHUB_REF are needed"])
        return 2
    try:
        open_alerts = alerts(repo, token, ref, "open")
        dismissed = alerts(repo, token, ref, "dismissed")
    except OSError as e:
        report("CodeQL: the verdict", [f"cannot read the alerts for {ref}: {e}"])
        return 2

    def path(a):
        return a["most_recent_instance"]["location"]["path"]

    def row(a):
        loc = a["most_recent_instance"]["location"]
        return (a["rule"]["severity"], a["rule"]["id"],
                f"{loc['path']}:{loc['start_line']}",
                a["most_recent_instance"]["message"]["text"].split("\n")[0][:120])

    ours = [a for a in open_alerts
            if not path(a).startswith(SMOOTHIE + SET_ASIDE) and not unsuited(a["rule"]["id"], path(a))]
    smoothie = [a for a in open_alerts if path(a).startswith(SMOOTHIE)]
    failing = sorted((row(a) for a in ours if a["rule"]["severity"] in FAILING),
                     key=lambda x: (ORDER.get(x[0], 9), x[1], x[2]))
    notes = len(ours) - len(failing)
    lines = [f"{len(ours)} alerts open on {ref} in Clockwork's code: {len(failing)} at warning or error, "
             f"{notes} notes; {len(dismissed)} dismissed as by design or false",
             f"{len(smoothie)} open in Smoothie, our fork to work down, not judged here"]
    if failing:
        lines += table(failing)
        lines += ["", "fix each, or dismiss it in the Security tab with the reason it stands"]
    report("CodeQL: the verdict", lines)
    return 1 if failing else 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["trim"] and len(sys.argv) in (4, 5):
        sys.exit(trim(*sys.argv[2:]))
    if sys.argv[1:] == ["judge"]:
        sys.exit(judge())
    sys.exit(__doc__)
