#!/usr/bin/env bash
# One disk's worth of a real-disk experiment (run by the benchmark host, see infra/bench). The output is filtered to fit the 64 KB serial
# console; the full output goes to $LOG when it is set.
#   vfs/real-disk.sh DIR [REPS] [MODE]
# MODE commit-path (default): the raw I/O patterns of one commit, and the SQLite benchmark on the configurations that matter for the
#                            commit-path question (docs/VFS-SPIKE.md, the gap to WAL).
# MODE sustain:              20,000 single-row commits in a row on WAL, the log as it was, and the preallocating log with 1, 4, 16 and
#                            64 MiB chunks: the latency tail, the I/O per commit including the zero fill, and the cost of a reopen.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
dir=${1:?usage: real-disk.sh DIR [REPS] [MODE]}
reps=${2:-3}
mode=${3:-commit-path}
keep() { if [ -n "${LOG:-}" ]; then tee "$LOG"; else cat; fi; }
case $mode in
  commit-path)
    echo "--- raw patterns of one commit (2 page records + 1 commit record, 8280 bytes)"
    timeout 600 "$here/build/commit_micro" "$dir" "$reps" 2000
    echo "--- SQLite benchmark"
    BENCH_ONLY='file/WAL forkdb/journal forkdb/prealloc forkdb/coalesce forkdb/both' timeout 1500 "$here/build/vfs_bench-batch" "$dir" "$reps" \
      | keep | grep -E '^R [0-9]+ [A-Za-z0-9/_.-]+ (write4k|w1_|w2_ms_per_1000_row_txn_p50|w7_fk_open|w6_fork_p50|file_bytes)'
    ;;
  sustain)
    echo "--- sustained commits"
    BENCH_MODE=sustain BENCH_ONLY='file/WAL forkdb/journal forkdb/pa-1M forkdb/pa-4M forkdb/pa-16M forkdb/pa-64M' timeout 3000 "$here/build/vfs_bench-batch" "$dir" "$reps" \
      | keep | grep -E '^R [0-9]+ [A-Za-z0-9/_.-]+ (sustain_|write4k)'
    ;;
  *) echo "unknown mode $mode (commit-path or sustain)" >&2; exit 2 ;;
esac
