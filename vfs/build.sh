#!/usr/bin/env bash
# Build helper for the VFS spike. Needs gcc, curl, sha256sum and python3.
#   vfs/build.sh sqlite          compile the pinned SQLite three times: stock; with SQLITE_ENABLE_BATCH_ATOMIC_WRITE; and 'session', stock plus the
#                                session extension (SQLITE_ENABLE_SESSION and SQLITE_ENABLE_PREUPDATE_HOOK), the build the CLI will ship (cached)
#   vfs/build.sh test [variant]  build vfs/build/vfs_test-<variant> (variant: batch, stock or session; default batch)
#   vfs/build.sh bench [variant] build vfs/build/vfs_bench-<variant>
#   vfs/build.sh micro           build vfs/build/commit_micro (raw I/O patterns of one commit; needs only gcc)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
src=$root/spike/vendor/sqlite-amalgamation-3500000
out=$here/build
mkdir -p "$out"
opt=${OPT:--O2}
cmd=${1:-}
variant=${2:-batch}

flags_for() {
  case $1 in
    stock) echo "-DSQLITE_OMIT_LOAD_EXTENSION" ;;
    session) echo "-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK" ;;
    batch) echo "-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_BATCH_ATOMIC_WRITE" ;;
    *) echo "unknown variant $1 (stock, batch or session)" >&2; exit 2 ;;
  esac
}

# The amalgamation's sqlite3.h does not declare the session API: its header is inside sqlite3.c, between two marker comments.
session_header() {
  local h=$out/sqlite3session.h s e
  [ -f "$h" ] && return 0
  s=$(grep -n 'Begin file sqlite3session.h' "$src/sqlite3.c" | head -1 | cut -d: -f1)
  e=$(grep -n 'End of sqlite3session.h' "$src/sqlite3.c" | head -1 | cut -d: -f1)
  [ -n "$s" ] && [ -n "$e" ] || { echo "no sqlite3session.h inside the amalgamation" >&2; exit 1; }
  sed -n "${s},${e}p" "$src/sqlite3.c" > "$h.tmp"
  mv "$h.tmp" "$h"
}

case $cmd in
  sqlite)
    "$root/spike/fetch-sqlite.sh"
    pids=()
    names=()
    session_header
    for v in stock batch session; do
      if [ -f "$out/sqlite3-$v.o" ]; then echo "cached: sqlite3-$v.o"; continue; fi
      # shellcheck disable=SC2046,SC2086  # the flags are a list of options on purpose
      gcc $opt -c $(flags_for "$v") "$src/sqlite3.c" -o "$out/sqlite3-$v.o.tmp" &
      pids[${#pids[@]}]=$!
      names[${#names[@]}]=$v
    done
    for i in "${!pids[@]}"; do
      wait "${pids[$i]}"
      mv "$out/sqlite3-${names[$i]}.o.tmp" "$out/sqlite3-${names[$i]}.o"
      echo "built sqlite3-${names[$i]}.o"
    done
    ;;
  test|bench)
    [ -f "$out/sqlite3-$variant.o" ] || { echo "run: $0 sqlite   first" >&2; exit 1; }
    session_header
    srcs="$here/fkvfs.c $root/store/fkstore.c $root/store/io_file.c $root/store/io_prealloc.c"
    extra=""
    # SAN=1: address and undefined-behaviour sanitizers on our code (SQLite itself is not instrumented)
    [ -n "${SAN:-}" ] && extra="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
    suffix=${SAN:+-san}
    [ "$cmd" = test ] && srcs="$here/vfs_test.c $srcs $root/test/simdisk.c" || srcs="$here/vfs_bench.c $srcs"
    # shellcheck disable=SC2086,SC2046  # lists of files and options on purpose
    gcc -Wall -Wextra -std=gnu11 -O2 $(flags_for "$variant") $extra -I"$src" -I"$out" -I"$root/store" $srcs "$out/sqlite3-$variant.o" -o "$out/vfs_${cmd}-$variant$suffix" -lpthread -lm
    echo "built $out/vfs_${cmd}-$variant$suffix"
    ;;
  micro)
    gcc -Wall -Wextra -std=gnu11 -O2 "$here/commit_micro.c" -o "$out/commit_micro"
    echo "built $out/commit_micro"
    ;;
  *) echo "usage: $0 sqlite | micro | test [stock|batch|session] | bench [stock|batch|session]" >&2; exit 2 ;;
esac
