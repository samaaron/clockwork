#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
"""Judge a CodeQL SARIF file by Clockwork's own code.

    codeql-report.py IN.sarif OUT.sarif

The analysis compiles everything, so its results cover Smoothie (our JUCE
fork), the vendored single-file libraries, the plugin SDKs and the fetched
dependencies as well as Clockwork. Those findings are their authors' and
would bury ours (345 of the first run's 443), and CodeQL's paths-ignore does
not apply to a compiled language built by hand, so the results are trimmed
here instead: OUT.sarif keeps only the findings in Clockwork's own sources,
and is what gets uploaded, so the Security tab shows the same set.

The report goes to the log and, when GITHUB_STEP_SUMMARY is set, to the job
summary. The exit code is 1 for a finding at error level in our code, and 2
when the file does not prove an analysis happened: no results at all, or a
rule whose severity cannot be found, is not a clean bill of health.
"""
import json
import os
import sys

THIRD_PARTY = ("smoothie/", "src/vendor/", "plugins/", "build/")


def severities(run):
    """Rule id -> level. CodeQL files its rules under the query-pack extensions;
    the upload API's copy moves them to the driver, so both are read."""
    out = {}
    tool = run.get("tool", {})
    for component in [tool.get("driver", {})] + tool.get("extensions", []):
        for rule in component.get("rules", []):
            out[rule["id"]] = rule.get("defaultConfiguration", {}).get("level", "warning")
    return out


def main(src, dst):
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

    order = {"error": 0, "?": 1, "warning": 2, "note": 3}
    rows.sort(key=lambda x: (order.get(x[0], 9), x[1], x[2]))
    errors = [x for x in rows if x[0] == "error"]
    seen = rows or dropped
    lines = [f"CodeQL: {len(rows)} findings in Clockwork's code, {len(errors)} at error level "
             f"({dropped} third-party findings set aside)"]
    if rows:
        lines += ["", "| level | rule | where | message |", "|---|---|---|---|"]
        lines += [f"| {l} | {r} | {w} | {m.replace('|', '/')} |" for l, r, w, m in rows]
    if not seen:
        lines.append("no results in the SARIF at all: the analysis did not look")
    if unknown:
        lines.append(f"{len(unknown)} findings with no severity on record: " + ", ".join(sorted(set(unknown))))
    text = "\n".join(lines)
    print(text)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as s:
            s.write("## CodeQL (c-cpp)\n\n" + text + "\n")
    if not seen or unknown:
        return 2
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
