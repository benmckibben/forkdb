#!/usr/bin/env bash
# Build the benchmark (optimised, no sanitizers) and run one scenario in a scratch directory.
#   scripts/bench-compact.sh shrink 1024 3 100      # 1 GiB live, 3 rounds, rounds 2..3 overwrite 100% of the pages
#   scripts/bench-compact.sh forks  1024 100 1 random 50
#   scripts/bench-compact.sh depth  256 1000
# BENCH_DIR (default /tmp/fkbench) must be on the file system you want to measure and have room for the log plus a second copy.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dir=${BENCH_DIR:-/tmp/fkbench}
mkdir -p "$dir"
cc -O2 -g -Wall -Wextra -std=gnu11 -o "$dir/fkbench" "$root/bench/fkbench.c" "$root/store/fkstore.c" "$root/store/io_file.c" -lpthread
exec "$dir/fkbench" "$1" "$dir" "${@:2}"
