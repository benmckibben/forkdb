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

# The power-loss model has 8 branch ids so that ids are reused all the time; the file tests use the default (1024).
crash_src="-DFK_MAX_BRANCHES=8 $here/test/crash_test.c $here/test/simdisk.c $here/store/fkstore.c $here/store/io_prealloc.c"
fdata=("-DFK_TEST_DATA=\"$here/test/data\"") # the path of the version 2 fixture, as a C string

echo "== real-file tests"
$cc $warn "${fdata[@]}" "$here/test/file_test.c" "$here/store/fkstore.c" "$here/store/io_file.c" "$here/store/io_prealloc.c" -o "$out/file_test"
"$out/file_test"

# The nosync test counts the fsync and fdatasync calls of io_file.c: that one file is built with test/nosync_redirect.h force-included (no linker option, so this also builds on macOS).
build_nosync_test() { # output [extra compiler flags]
  local o=$1; shift
  # shellcheck disable=SC2086
  $cc $warn "$@" -include "$here/test/nosync_redirect.h" -c "$here/store/io_file.c" -o "$out/io_file_counted.o"
  # shellcheck disable=SC2086
  $cc $warn "$@" "$here/test/nosync_test.c" "$here/store/fkstore.c" "$here/store/io_prealloc.c" "$out/io_file_counted.o" -o "$o"
}
echo "== nosync test"
build_nosync_test "$out/nosync_test"
"$out/nosync_test"

echo "== power-loss tests (${seeds} seeds x ${ops} operations, address and undefined-behaviour sanitizers on)"
# shellcheck disable=SC2086
$cc $warn -fsanitize=address,undefined -fno-sanitize-recover=undefined $crash_src -o "$out/crash_test"
"$out/crash_test" "$seeds" "$ops" | grep -E "^(logic pass|no-sync pass|workloads|post-crash|append-after|TOTAL)"

echo "== the same power-loss tests with every store behind the preallocation layer (16 KiB chunks, so the cuts include the zero fills)"
CRASH_PREALLOC=16384 "$out/crash_test" "$seeds" "$ops" | grep -E "^(every store|logic pass|no-sync pass|workloads|post-crash|append-after|TOTAL)"

[ "${1:-}" = quick ] && { echo "all store tests passed (quick)"; exit 0; }

echo "== do the tests catch broken stores? (each mutant must be caught by at least one test)"
mutant() { # name, flags, [env for the power-loss run, e.g. CRASH_PREALLOC=16384]
  local name=$1 flags=$2 cenv=${3:-} c=no f=no
  # shellcheck disable=SC2086
  $cc $warn $flags $crash_src -o "$out/m_crash"
  # shellcheck disable=SC2086
  $cc $warn $flags "${fdata[@]}" "$here/test/file_test.c" "$here/store/fkstore.c" "$here/store/io_file.c" "$here/store/io_prealloc.c" -o "$out/m_file"
  # A broken store may hang instead of failing (an endless loop): five minutes is far more than a healthy run needs (a few
  # seconds for the file tests, under two minutes for the power-loss run), and a hang counts as caught.
  env $cenv timeout 300 "$out/m_crash" 25 8 >/dev/null 2>&1 || c=YES
  timeout 300 "$out/m_file" >/dev/null 2>&1 || f=YES
  printf "  %-52s power-loss harness: %-3s  file tests: %s\n" "$name" "$c" "$f"
  [ "$c" = YES ] || [ "$f" = YES ]
}
mutant "commit acknowledged without fsync" "-DFK_BUG_SKIP_FSYNC"
mutant "durable-end marker written before its data is synced" "-DFK_BUG_EARLY_MARKER"
mutant "recovery does not truncate the torn tail" "-DFK_BUG_NO_TRUNC"
mutant "recovery does not check the record chain" "-DFK_BUG_NO_PREVCHAIN"
mutant "neither of the previous two" "-DFK_BUG_NO_TRUNC -DFK_BUG_NO_PREVCHAIN"
mutant "compaction: new file swapped in without being synced" "-DFK_BUG_COMPACT_NO_TMP_SYNC"
mutant "compaction: directory not synced after the rename" "-DFK_BUG_COMPACT_NO_DIRSYNC"
mutant "compaction: tombstones dropped too early" "-DFK_BUG_COMPACT_DROP_TOMB"
mutant "compaction: copies a damaged page without checking" "-DFK_BUG_COMPACT_NO_VERIFY"
mutant "open: leftover compaction file is not deleted" "-DFK_BUG_COMPACT_NO_CLEANUP"
mutant "file: two openers are not refused (no file lock)" "-DFK_BUG_NO_FILE_LOCK"
mutant "hardware CRC32C leaves out the last bytes of a buffer" "-DFK_BUG_CRC_HW_TAIL"
mutant "index: a rehash loses one entry" "-DFK_BUG_INDEX_GROW_DROP"
mutant "index: a collision is treated as a miss" "-DFK_BUG_INDEX_NO_PROBE"
mutant "index: overwriting a page adds a second entry" "-DFK_BUG_INDEX_DUP_ON_OVERWRITE"
mutant "ids: fork can hand out the id of a live branch" "-DFK_BUG_ID_REUSE_ALIVE"
mutant "layers: a fold lets the parent's entry win" "-DFK_BUG_FOLD_PARENT_WINS"
mutant "layers: a fold leaves the wrong size" "-DFK_BUG_FOLD_SIZE"
mutant "layers: a fold leaves a branch on the freed layer" "-DFK_BUG_FOLD_NO_RETARGET"
mutant "layers: a fold does not carry tombstones over" "-DFK_BUG_FOLD_DROP_TOMB"
mutant "layers: a layer with two children is folded too" "-DFK_BUG_FOLD_SHARED"
mutant "layers: a layer others build on is freed" "-DFK_BUG_LAYER_FREE_SHARED"
mutant "layers: discard never frees layers" "-DFK_BUG_LAYER_NO_FREE"
mutant "layers: discard never folds single-child layers" "-DFK_BUG_LAYER_NO_FOLD"
mutant "ids: recovery rejects a fork that reuses an id" "-DFK_BUG_ID_REPLAY_STRICT"
mutant "ids: the allocation cursor is not restored from a snapshot" "-DFK_BUG_ID_NO_CURSOR"
mutant "v4: a commit leaves the state id as it was" "-DFK_BUG_STATE_COMMIT_NOT_BUMPED"
mutant "v4: a fork gets a state id of its own" "-DFK_BUG_STATE_FORK_NOT_COPIED"
mutant "v4: a reset leaves the branch with its old content" "-DFK_BUG_RESET_NO_EFFECT"
mutant "v4: a reset gives the branch a state id of its own" "-DFK_BUG_STATE_RESET_NOT_COPIED"
mutant "v4: the state counter is not restored from a snapshot" "-DFK_BUG_STATE_CTR_NOT_RESTORED"
mutant "v4: a version 3 snapshot gives every branch the same state id" "-DFK_BUG_V3_SNAP_STATES_EQUAL"
mutant "v4: a discarded branch leaves its metadata to the next one" "-DFK_BUG_META_KEPT_ON_DISCARD"
mutant "v4: recovery forgets the metadata a fork was given" "-DFK_BUG_FORKM_REPLAY_NO_META"
mutant "v4: compaction leaves the metadata out" "-DFK_BUG_META_LOST_COMPACT"
mutant "prealloc: the header is preallocated too (a cut leaves zeros)" "-DFK_BUG_PA_EXTEND_HEADER" "CRASH_PREALLOC=16384"
mutant "prealloc: the zero fill starts at the file start" "-DFK_BUG_PA_FILL_FROM_ZERO" "CRASH_PREALLOC=16384"
mutant "prealloc: the zero fill runs after the data is written" "-DFK_BUG_PA_FILL_AFTER_WRITE" "CRASH_PREALLOC=16384"
mutant "prealloc: the state is forgotten after a compaction" "-DFK_BUG_PA_RENAME_FORGETS_STATE" "CRASH_PREALLOC=16384"
mutant "prealloc: the trim cuts one byte too many" "-DFK_BUG_PA_TRIM_SHORT" "CRASH_PREALLOC=16384"
mutant "prealloc: a failed zero fill fails the write" "-DFK_BUG_PA_NO_FALLBACK" "CRASH_PREALLOC=16384"

# nosync mutant: the bug makes nosync still issue syncs, so the nosync check (counter must stay at 0) fails
nosync_mutant() {
  local flags="-DFK_BUG_NOSYNC_STILL_SYNCS"
  build_nosync_test "$out/m_nosync" $flags
  timeout 120 "$out/m_nosync" >/dev/null 2>&1 && { printf "  %-52s ALIVE (not killed)\n" "nosync still syncs"; return 1; }
  printf "  %-52s KILLED\n" "nosync still syncs"
}
nosync_mutant

echo "all store tests passed"
