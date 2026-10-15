#!/usr/bin/env bash
# Runs the isolation canaries in every mode that could expose a leak; exits nonzero if any mode misbehaves.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root" || exit 1
export PYTHONPATH="$root${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONDONTWRITEBYTECODE=1
py=(python3 -m pytest -p no:cacheprovider -q canary)
fail=0
run() { # name expected-exit-code expected-summary-regex env...
  local name=$1 want=$2 re=$3; shift 3
  out=$(env "$@" "${py[@]}" 2>&1); rc=$?
  if [ "$rc" = "$want" ] && grep -Eq "$re" <<< "$out"; then echo "ok   $name: $(tail -1 <<< "$out")"; else echo "FAIL $name: rc=$rc (want $want)"; tail -15 <<< "$out"; fail=1; fi
}
run "in order"                0 '^[0-9]+ passed in' FORKDB_ORDER=
run "reversed"                0 '^[0-9]+ passed in' FORKDB_ORDER=reverse
run "shuffled, seed 1"        0 '^[0-9]+ passed in' FORKDB_ORDER=shuffle FORKDB_SEED=1
run "shuffled, seed 2"        0 '^[0-9]+ passed in' FORKDB_ORDER=shuffle FORKDB_SEED=2
run "4 workers"               0 '^[0-9]+ passed in' FORKDB_ORDER=shuffle PYTEST_ADDOPTS="-n 4"
run "4 workers, tmpfs"        0 '^[0-9]+ passed in' FORKDB_ORDER=reverse PYTEST_ADDOPTS="-n 4 --forkdb-dir=/dev/shm/fk-canary"
out=$(env FORKDB_ORDER= python3 -m pytest -p no:cacheprovider -q canary/failing_case.py 2>&1); rc=$?
if [ "$rc" = 1 ] && grep -Eq '^1 failed, 1 passed' <<< "$out"; then echo "ok   a failing test, then the next one: $(tail -1 <<< "$out")"; else echo "FAIL a failing test, then the next one: rc=$rc"; tail -15 <<< "$out"; fail=1; fi
run "tiny size limit: compaction between tests" 0 '^[0-9]+ passed in' PYTEST_ADDOPTS="--forkdb-max-mb=1"
exit $fail
