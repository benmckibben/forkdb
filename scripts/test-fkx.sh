#!/usr/bin/env bash
# Build and run the fkx tests: the branch layer over the store and the VFS. Needs gcc, curl, sha256sum and python3 (the first run
# compiles the pinned SQLite, about a minute).
#   scripts/test-fkx.sh          the file tests, the power-loss test, then the harness self-check (mutants: each must make a test fail)
#   scripts/test-fkx.sh quick    the file tests and a shorter power-loss test, no mutants
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root" || exit 1
quick=0
[ "${1:-}" = quick ] && quick=1
out=${FKX_OUT:-$root/fkx/build}
mkdir -p "$out"
vfs/build.sh sqlite > "$out/sqlite.log" 2>&1 || { cat "$out/sqlite.log"; echo "building SQLite failed"; exit 1; }
src=spike/vendor/sqlite-amalgamation-3500000
srcs=(fkx/fkx.c fkx/fkx_nosession.c vfs/fkvfs.c store/fkstore.c store/io_file.c store/io_prealloc.c)
warn=(-Wall -Wextra -std=gnu11 -O1 -g)
inc=("-I$src" -Istore -Ivfs -Ifkx)
san=('-fsanitize=address,undefined' -fno-sanitize-recover=undefined)

build() { # name main-file extra-flags... (the branch limit is lowered so that the id limit can be reached and the harness stays fast)
  local name=$1 main=$2; shift 2
  gcc "${warn[@]}" "$@" "${inc[@]}" "$main" test/simdisk.c "${srcs[@]}" vfs/build/sqlite3-stock.o -o "$out/$name" -lpthread -lm
}

echo '== build'
build fkx_test test/fkx_test.c "${san[@]}" -DFK_MAX_BRANCHES=16 "-DFK_TEST_DATA=\"$root/test/data\"" || exit 1
build fkx_crash test/fkx_crash_test.c "${san[@]}" -DFK_MAX_BRANCHES=64 || exit 1
echo '== file tests'
timeout 300 "$out/fkx_test" | tail -3 || exit 1
echo '== power loss'
seeds=40; ops=14
[ "$quick" = 1 ] && { seeds=10; ops=12; }
timeout 3000 "$out/fkx_crash" "$seeds" "$ops" | tail -4 || exit 1
[ "$quick" = 1 ] && { echo 'all fkx tests passed (quick: no mutants)'; exit 0; }

echo '== mutants: each must make a test fail'
survived=0
mutant() { # description define
  local desc=$1 def=$2 f=no c=no
  build m_file test/fkx_test.c -DFK_MAX_BRANCHES=16 "-DFK_TEST_DATA=\"$root/test/data\"" "-D$def" 2> /dev/null
  build m_crash test/fkx_crash_test.c -DFK_MAX_BRANCHES=64 "-D$def" 2> /dev/null
  timeout 300 "$out/m_file" > /dev/null 2>&1 || f=YES
  timeout 600 "$out/m_crash" 10 12 > /dev/null 2>&1 || c=YES
  printf '%-92s file tests: %-3s power loss: %s\n' "$desc" "$f" "$c"
  if [ "$f" = no ] && [ "$c" = no ]; then echo "  SURVIVED: $def"; survived=$((survived + 1)); fi
}
mutant 'a branch whose metadata says read-only can be opened for writing' FKX_BUG_GUARD_ALLOWS_WRITES
mutant 'two branches can have the same name' FKX_BUG_NAMES_NOT_UNIQUE
mutant 'a fork is made before its base (a cut leaves a fork with no base)' FKX_BUG_FORK_BEFORE_BASE
mutant 'a discard removes the base before the fork (a cut leaves a live fork with a dead base)' FKX_BUG_DISCARD_BASE_FIRST
mutant 'a discard leaves the base behind' FKX_BUG_DISCARD_LEAVES_BASE
mutant 'a reset is allowed with a connection open on the branch' FKX_BUG_RESET_IGNORES_OPEN
mutant 'gc collects the undo points the owner lists' FKX_BUG_GC_TAKES_LISTED_UNDO
mutant 'a promote fast-forwards without checking that the target has not moved' FKX_BUG_FF_WITHOUT_STATE_CHECK
mutant 'a promote takes its undo snapshot after the reset' FKX_BUG_RESET_BEFORE_UNDO_SNAPSHOT
mutant 'an undo does not check that the target is where the promote left it' FKX_BUG_UNDO_WITHOUT_STATE_CHECK
if [ "$survived" -ne 0 ]; then echo "$survived mutant(s) survived"; exit 1; fi
echo 'all fkx tests passed'
