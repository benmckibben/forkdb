#!/usr/bin/env bash
# shellcheck disable=SC2086  # $warn is a list of options on purpose
# Build and run the durable-store tests. Needs only gcc.
#   scripts/test-store.sh            unit + file tests, power-loss tests, and the harness self-check (mutants)
#   scripts/test-store.sh quick      same with fewer workloads
#   SEEDS=300 OPS=10 scripts/test-store.sh   a longer power-loss run
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc=${CC:-gcc}
warn="-Wall -Wextra -std=gnu11 -O2"
seeds=${SEEDS:-60}
ops=${OPS:-8}
[ "${1:-}" = quick ] && seeds=10

crash_src="$here/test/crash_test.c $here/test/simdisk.c $here/store/fkstore.c"

echo "== real-file tests"
$cc $warn "$here/test/file_test.c" "$here/store/fkstore.c" "$here/store/io_file.c" -o "$out/file_test"
"$out/file_test"

echo "== power-loss tests (${seeds} seeds x ${ops} operations, address and undefined-behaviour sanitizers on)"
# shellcheck disable=SC2086
$cc $warn -fsanitize=address,undefined -fno-sanitize-recover=undefined $crash_src -o "$out/crash_test"
"$out/crash_test" "$seeds" "$ops" | tail -6

echo "== do the tests catch broken stores? (each mutant must be caught by at least one test)"
mutant() { # name, flags
  local name=$1 flags=$2 c=no f=no
  # shellcheck disable=SC2086
  $cc $warn $flags $crash_src -o "$out/m_crash"
  # shellcheck disable=SC2086
  $cc $warn $flags "$here/test/file_test.c" "$here/store/fkstore.c" "$here/store/io_file.c" -o "$out/m_file"
  "$out/m_crash" 25 8 >/dev/null 2>&1 || c=YES
  "$out/m_file" >/dev/null 2>&1 || f=YES
  printf "  %-52s power-loss harness: %-3s  file tests: %s\n" "$name" "$c" "$f"
  [ "$c" = YES ] || [ "$f" = YES ]
}
mutant "commit acknowledged without fsync" "-DFK_BUG_SKIP_FSYNC"
mutant "durable-end marker written before its data is synced" "-DFK_BUG_EARLY_MARKER"
mutant "recovery does not truncate the torn tail" "-DFK_BUG_NO_TRUNC"
mutant "recovery does not check the record chain" "-DFK_BUG_NO_PREVCHAIN"
mutant "neither of the previous two" "-DFK_BUG_NO_TRUNC -DFK_BUG_NO_PREVCHAIN"
echo "all store tests passed"
