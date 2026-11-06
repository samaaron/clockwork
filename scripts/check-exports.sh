#!/usr/bin/env bash
# A shared module exports its entry points and nothing else.
#
#   scripts/check-exports.sh <module> <entry>...
#
# Lists the module's defined, external symbols (nm) and fails on any that is
# not an entry named. Prints what it looked at, so a run that found nothing
# to check cannot pass in silence. Mach-O names carry a leading underscore,
# which is dropped before comparing.
set -euo pipefail
module="${1:?module path}"; shift
[ $# -gt 0 ] || { echo "check-exports: no entry points named" >&2; exit 2; }
[ -f "$module" ] || { echo "check-exports: no such module: $module" >&2; exit 2; }

case "$(uname -s)" in
    Darwin) exported=$(nm -gU "$module" | awk '$2 ~ /^[TDBSR]$/ {sub(/^_/, "", $3); print $3}') ;;
    *)      exported=$(nm -D --defined-only "$module" | awk '$2 ~ /^[TDBRVWi]$/ {print $3}') ;;
esac
count=$(printf '%s\n' "$exported" | grep -c . || true)

status=0
for entry in "$@"; do
    if printf '%s\n' "$exported" | grep -qx "$entry"; then
        echo "check-exports: $module exports $entry"
    else
        echo "check-exports: $module does not export $entry" >&2
        status=1
    fi
done
others=$(printf '%s\n' "$exported" | grep -vxF -f <(printf '%s\n' "$@") | grep . || true)
if [ -n "$others" ]; then
    echo "check-exports: $module exports $(printf '%s\n' "$others" | wc -l | tr -d ' ') symbol(s) beyond its entry point(s), such as:" >&2
    printf '%s\n' "$others" | sed -n '1,8p' | sed 's/^/  /' >&2
    status=1
fi
echo "check-exports: $count exported symbol(s) looked at"
exit $status
