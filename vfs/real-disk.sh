#!/usr/bin/env bash
# One disk's worth of the real-disk experiment (run by the benchmark host, see infra/bench): the SQLite benchmark on the configurations that
# matter for the commit-path question, and the raw I/O patterns of one commit. The output is filtered to fit the 64 KB serial console;
# the full output goes to $LOG when it is set.
#   vfs/real-disk.sh DIR [REPS]
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
dir=${1:?usage: real-disk.sh DIR [REPS]}
reps=${2:-3}
echo "--- raw patterns of one commit (2 page records + 1 commit record, 8280 bytes)"
timeout 600 "$here/build/commit_micro" "$dir" "$reps" 2000
echo "--- SQLite benchmark"
BENCH_ONLY='file/WAL forkdb/journal forkdb/prealloc forkdb/coalesce forkdb/both' timeout 1500 "$here/build/vfs_bench-batch" "$dir" "$reps" \
  | { if [ -n "${LOG:-}" ]; then tee "$LOG"; else cat; fi; } \
  | grep -E '^R [0-9]+ (disk|[a-zA-Z/]+) (write4k|w1_|w2_ms_per_1000_row_txn_p50|w7_fk_open|w6_fork_p50|file_bytes)'
