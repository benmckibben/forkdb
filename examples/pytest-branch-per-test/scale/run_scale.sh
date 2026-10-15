#!/usr/bin/env bash
# 200 tests against a large seeded database under each isolation strategy; prints wall-clock time of the whole pytest process.
#   scale/run_scale.sh [rows ...]     (default 20000 200000 1000000)       needs pytest; DIR=/dev/shm/x puts the files on tmpfs
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
export PYTHONPATH="$root${PYTHONPATH:+:$PYTHONPATH}" PYTHONDONTWRITEBYTECODE=1
dir=${DIR:-/tmp/fk-scale}
mkdir -p "$dir"
sizes=("$@"); [ ${#sizes[@]} -gt 0 ] || sizes=(20000 200000 1000000)
echo "200 tests (100 reads, 60 committed writes, 40 uncommitted writes); files in $dir; whole pytest process, seconds"
for rows in "${sizes[@]}"; do
  printf "%9d rows:" "$rows"
  for plugin in forkdb copy reseed; do
    if [ "$plugin" = reseed ] && [ "$rows" -gt 200000 ]; then printf "  %s n/a (too slow)" "$plugin"; continue; fi
    s=$(date +%s.%N)
    out=$(cd "$here" && SCALE_PLUGIN=$plugin SCALE_ROWS=$rows python3 -m pytest -p no:cacheprovider -q --forkdb-dir="$dir" test_scale.py 2>&1); rc=$?
    e=$(date +%s.%N)
    if [ $rc -ne 0 ]; then printf "  %s FAILED" "$plugin"; echo; tail -5 <<< "$out"; exit 1; fi
    printf "  %s %6.2f" "$plugin" "$(echo "$e - $s" | bc)"
  done
  echo
done
