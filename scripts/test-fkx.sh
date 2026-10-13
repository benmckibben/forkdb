#!/usr/bin/env bash
# Build and run the fkx tests: the branch layer over the store and the VFS. Needs gcc, curl, sha256sum and python3 (the first run
# compiles the pinned SQLite, about a minute). Two builds: without the session driver (the core, on the stock SQLite) and with it (on the
# SQLite built with the session extension).
#   scripts/test-fkx.sh          both builds: file tests, power-loss tests, then the harness self-check (mutants: each must make a test fail)
#   scripts/test-fkx.sh quick    both builds: file tests and a shorter power-loss test, no mutants
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
common=(fkx/fkx.c vfs/fkvfs.c store/fkstore.c store/io_file.c store/io_prealloc.c)
warn=(-Wall -Wextra -std=gnu11 -O1 -g)
inc=("-I$src" -Ivfs/build -Istore -Ivfs -Ifkx)
san=('-fsanitize=address,undefined' -fno-sanitize-recover=undefined)
sess=(-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK)

# build variant name main-file extra-flags...   (variant: core or session; the branch limit is set by the caller so that the id limit can be
# reached and the harnesses stay fast)
build() {
  local variant=$1 name=$2 main=$3; shift 3
  if [ "$variant" = session ]; then
    gcc "${warn[@]}" "${sess[@]}" "$@" "${inc[@]}" "$main" test/simdisk.c "${common[@]}" fkx/fkx_session.c vfs/build/sqlite3-session.o -o "$out/$name" -lpthread -lm
  else
    gcc "${warn[@]}" "$@" "${inc[@]}" "$main" test/simdisk.c "${common[@]}" fkx/fkx_nosession.c vfs/build/sqlite3-stock.o -o "$out/$name" -lpthread -lm
  fi
}

data="-DFK_TEST_DATA=\"$root/test/data\""
seeds=40; ops=14; sseeds=30; sops=20
[ "$quick" = 1 ] && { seeds=10; ops=12; sseeds=10; sops=16; }

echo '== build'
build core fkx_test test/fkx_test.c "${san[@]}" -DFK_MAX_BRANCHES=16 "$data" || exit 1
build core fkx_crash test/fkx_crash_test.c "${san[@]}" -DFK_MAX_BRANCHES=64 || exit 1
build session fkx_session_test test/fkx_session_test.c "${san[@]}" -DFK_MAX_BRANCHES=64 || exit 1
build session fkx_session_crash test/fkx_session_crash_test.c "${san[@]}" -DFK_MAX_BRANCHES=64 || exit 1
echo '== core: file tests (no session driver: replays answer FKX_ENOSESSION)'
timeout 300 "$out/fkx_test" | tail -3 || exit 1
echo '== core: power loss'
timeout 3000 "$out/fkx_crash" "$seeds" "$ops" | tail -4 || exit 1
echo '== session driver: file tests'
timeout 300 "$out/fkx_session_test" | tail -3 || exit 1
echo '== session driver: power loss (promote by replay and fast-forward, undo exact and by replay)'
timeout 3000 "$out/fkx_session_crash" "$sseeds" "$sops" | tail -5 || exit 1
[ "$quick" = 1 ] && { echo 'all fkx tests passed (quick: no mutants)'; exit 0; }

echo '== mutants: each must make a test fail'
survived=0
mutant() { # variant description define
  local variant=$1 desc=$2 def=$3 f=no c=no
  if [ "$variant" = core ]; then
    build core m_file test/fkx_test.c -DFK_MAX_BRANCHES=16 "$data" "-D$def" 2> /dev/null
    build core m_crash test/fkx_crash_test.c -DFK_MAX_BRANCHES=64 "-D$def" 2> /dev/null
    timeout 300 "$out/m_file" > /dev/null 2>&1 || f=YES
    timeout 600 "$out/m_crash" 10 12 > /dev/null 2>&1 || c=YES
  else
    build session m_file test/fkx_session_test.c -DFK_MAX_BRANCHES=64 "-D$def" 2> /dev/null
    build session m_crash test/fkx_session_crash_test.c -DFK_MAX_BRANCHES=64 "-D$def" 2> /dev/null
    timeout 300 "$out/m_file" > /dev/null 2>&1 || f=YES
    timeout 900 "$out/m_crash" 12 20 > /dev/null 2>&1 || c=YES
  fi
  printf '%-7s %-100s file tests: %-3s power loss: %s\n' "$variant" "$desc" "$f" "$c"
  if [ "$f" = no ] && [ "$c" = no ]; then echo "  SURVIVED: $def"; survived=$((survived + 1)); fi
}
mutant core 'a branch whose metadata says read-only can be opened for writing' FKX_BUG_GUARD_ALLOWS_WRITES
mutant core 'two branches can have the same name' FKX_BUG_NAMES_NOT_UNIQUE
mutant core 'a fork is made before its base (a cut leaves a fork with no base)' FKX_BUG_FORK_BEFORE_BASE
mutant core 'a discard removes the base before the fork (a cut leaves a live fork with a dead base)' FKX_BUG_DISCARD_BASE_FIRST
mutant core 'a discard leaves the base behind' FKX_BUG_DISCARD_LEAVES_BASE
mutant core 'a reset is allowed with a connection open on the branch' FKX_BUG_RESET_IGNORES_OPEN
mutant core 'gc collects the undo points the owner lists' FKX_BUG_GC_TAKES_LISTED_UNDO
mutant core 'a promote fast-forwards without checking that the target has not moved' FKX_BUG_FF_WITHOUT_STATE_CHECK
mutant core 'a promote takes its undo snapshot after the reset' FKX_BUG_RESET_BEFORE_UNDO_SNAPSHOT
mutant core 'an undo does not check that the target is where the promote left it' FKX_BUG_UNDO_WITHOUT_STATE_CHECK
mutant session 'the schema policy does not notice a change of the schema (DDL)' FKX_BUG_POLICY_IGNORES_DDL
mutant session 'the schema policy does not notice changed rows of a table without a primary key' FKX_BUG_POLICY_IGNORES_NO_PK
mutant session 'the schema policy does not compare the table definitions of the target with those of the fork point' FKX_BUG_POLICY_IGNORES_TARGET_SCHEMA
mutant session 'a replay leaves the triggers of the target on (they fire again for rows the changeset carries)' FKX_BUG_REPLAY_KEEPS_TRIGGERS
mutant session 'a replay does not put the triggers it dropped back' FKX_BUG_TRIGGERS_NOT_RECREATED
mutant session 'a replay does not check the foreign keys before it commits' FKX_BUG_REPLAY_NO_FK_CHECK
mutant session 'a conflicting change is skipped and the rest applied' FKX_BUG_CONFLICT_OMITS
mutant session 'an undo by replay applies the promote again instead of its inverse' FKX_BUG_UNDO_REPLAY_WRONG_DIRECTION
mutant session 'a promote by replay records no undo point' FKX_BUG_REPLAY_NO_UNDO_RECORD
mutant session 'an undo by replay removes its entry after the replay instead of before (a cut leaves it undone and unrepeatable)' FKX_BUG_UNDO_ENTRY_AFTER_REPLAY
if [ "$survived" -ne 0 ]; then echo "$survived mutant(s) survived"; exit 1; fi
echo 'all fkx tests passed'
