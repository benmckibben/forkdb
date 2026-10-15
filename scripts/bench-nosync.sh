#!/usr/bin/env bash
# What nosync saves: time and durability calls per operation, durable against nosync, on the disk that FK_BENCH_DIR (default: the temp directory) is on.
# Needs Node 24 (node:sqlite) and what scripts/build-ext.sh needs. Prints a Markdown table; in CI it also goes to the job summary. Asserts only that nosync makes no
# durability calls and a durable store at least one per operation, never anything about time (see bench/nosync_bench.js).
#   scripts/bench-nosync.sh                 FK_BENCH_DIR=/some/disk scripts/bench-nosync.sh
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
node=${NODE:-node}

scripts/build-ext.sh > /dev/null
FK_EXT_SO=$(scripts/build-ext.sh --path)
export FK_EXT_SO

dir=${FK_BENCH_DIR:-${TMPDIR:-${RUNNER_TEMP:-/tmp}}}
echo "disk: $dir"
{ df -T "$dir" 2> /dev/null || df "$dir" 2> /dev/null || true; } | tail -n 2

out=$(mktemp)
status=0
"$node" --no-warnings bench/nosync_bench.js > "$out" || status=$?
cat "$out"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### nosync: ${RUNNER_OS:-$(uname -s)}"
        echo
        echo "disk: \`$dir\`"
        echo
        cat "$out"
        echo
    } >> "$GITHUB_STEP_SUMMARY"
fi
rm -f "$out"
exit "$status"
