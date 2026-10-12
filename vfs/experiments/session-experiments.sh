#!/usr/bin/env bash
# Run the experiments behind docs/SESSION-DESIGN.md. Needs gcc, curl, sha256sum and python3 (for the SQLite fetch).
#   vfs/experiments/session-experiments.sh          X1 to X6: diff through ATTACH, cross-branch writes, schema drift, triggers and foreign keys,
#                                                   replay conflicts and undo (a minute)
#   vfs/experiments/session-experiments.sh x7       cost of sqlite3session_diff against table size
#   vfs/experiments/session-experiments.sh bench    cost of the session build (about 4 minutes)
# Scratch files go to $FK_EXP_DIR (default /tmp/forkdb-session-exp). Builds a SQLite with SQLITE_ENABLE_SESSION into vfs/build (cached).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
src=$root/spike/vendor/sqlite-amalgamation-3500000
work=${FK_EXP_DIR:-/tmp/forkdb-session-exp}
out=$root/vfs/build
mode=${1:-exp}
mkdir -p "$work" "$out"
"$root/spike/fetch-sqlite.sh" > /dev/null

# The amalgamation's sqlite3.h has no session API: its header is inside sqlite3.c, between two markers.
s=$(grep -n 'Begin file sqlite3session.h' "$src/sqlite3.c" | head -1 | cut -d: -f1)
e=$(grep -n 'End of sqlite3session.h' "$src/sqlite3.c" | head -1 | cut -d: -f1)
sed -n "${s},${e}p" "$src/sqlite3.c" > "$work/sqlite3session.h"

build_obj() { # name flags...
  local name=$1; shift
  [ -f "$out/sqlite3-$name.o" ] && return 0
  gcc -O2 -c -DSQLITE_OMIT_LOAD_EXTENSION "$@" "$src/sqlite3.c" -o "$out/sqlite3-$name.o.tmp"
  mv "$out/sqlite3-$name.o.tmp" "$out/sqlite3-$name.o"
}
sess_flags=(-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK)
inc=("-I$src" "-I$work" "-I$root/store" "-I$root/vfs")
lib=("$root/store/fkstore.c" "$root/store/io_file.c" "$root/store/io_prealloc.c")

case $mode in
  exp|x7)
    build_obj session "${sess_flags[@]}"
    gcc -Wall -Wextra -std=gnu11 -O2 "${sess_flags[@]}" "${inc[@]}" -DFK_EXP_DIR="\"$work/\"" "$here/session_exp.c" "$root/vfs/fkvfs.c" "${lib[@]}" "$out/sqlite3-session.o" -o "$work/exp" -lpthread -lm
    if [ "$mode" = x7 ]; then "$work/exp" x7; exit 0; fi
    for x in x1 x2 x4 x5 x5b x5c x6; do timeout 120 "$work/exp" "$x"; done
    # the same cross-branch write with a VFS that refuses super-journal files (one added line)
    sed 's|  if (pOut) \*pOut = flags;|  if (pOut) *pOut = flags;\n  if (flags \& SQLITE_OPEN_SUPER_JOURNAL) return SQLITE_CANTOPEN;|' "$root/vfs/fkvfs.c" > "$work/fkvfs_refuse.c"
    grep -q SQLITE_OPEN_SUPER_JOURNAL "$work/fkvfs_refuse.c"
    gcc -Wall -std=gnu11 -O2 "${sess_flags[@]}" "${inc[@]}" -DFK_EXP_DIR="\"$work/\"" "$here/session_exp.c" "$work/fkvfs_refuse.c" "${lib[@]}" "$out/sqlite3-session.o" -o "$work/exp_refuse" -lpthread -lm
    echo '== the same X2/X3 with a VFS that refuses super-journal files'
    timeout 120 "$work/exp_refuse" x2
    ;;
  bench)
    build_obj stock
    build_obj session "${sess_flags[@]}"
    gcc -std=gnu11 -O2 "${inc[@]}" -DFK_EXP_DIR="\"$work/\"" "$here/session_hookbench.c" "$root/vfs/fkvfs.c" "${lib[@]}" "$out/sqlite3-stock.o" -o "$work/hb_stock" -lpthread -lm
    gcc -std=gnu11 -O2 "${sess_flags[@]}" -DWITH_SESSION "${inc[@]}" -DFK_EXP_DIR="\"$work/\"" "$here/session_hookbench.c" "$root/vfs/fkvfs.c" "${lib[@]}" "$out/sqlite3-session.o" -o "$work/hb_sess" -lpthread -lm
    echo 'mode variant rep milliseconds   (mem: 1,000,000 inserts and an update of a third of them in one transaction, in memory; commit: 3,000 single-row commits through the VFS)'
    for mode in mem commit; do
      for rep in 1 2 3 4 5; do
        echo "$mode stock $rep $(timeout 120 "$work/hb_stock" off "$mode")"
        echo "$mode sess_off $rep $(timeout 120 "$work/hb_sess" off "$mode")"
        echo "$mode sess_on $rep $(timeout 120 "$work/hb_sess" on "$mode")"
      done
    done
    ;;
  *) echo "usage: $0 [exp|x7|bench]" >&2; exit 2 ;;
esac
