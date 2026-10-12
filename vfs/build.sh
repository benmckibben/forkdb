#!/usr/bin/env bash
# Build helper for the VFS spike. Needs gcc, curl, sha256sum and python3.
#   vfs/build.sh sqlite          compile the pinned SQLite twice: stock, and with SQLITE_ENABLE_BATCH_ATOMIC_WRITE (cached; takes minutes)
#   vfs/build.sh test [variant]  build vfs/build/vfs_test-<variant> (variant: batch or stock; default batch)
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
    batch) echo "-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_BATCH_ATOMIC_WRITE" ;;
    *) echo "unknown variant $1 (stock or batch)" >&2; exit 2 ;;
  esac
}

case $cmd in
  sqlite)
    "$root/spike/fetch-sqlite.sh"
    pids=()
    names=()
    for v in stock batch; do
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
    srcs="$here/fkvfs.c $root/store/fkstore.c $root/store/io_file.c"
    extra=""
    # SAN=1: address and undefined-behaviour sanitizers on our code (SQLite itself is not instrumented)
    [ -n "${SAN:-}" ] && extra="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
    suffix=${SAN:+-san}
    [ "$cmd" = test ] && srcs="$here/vfs_test.c $srcs $root/test/simdisk.c" || srcs="$here/vfs_bench.c $srcs"
    # shellcheck disable=SC2086,SC2046  # lists of files and options on purpose
    gcc -Wall -Wextra -std=gnu11 -O2 $(flags_for "$variant") $extra -I"$src" -I"$root/store" $srcs "$out/sqlite3-$variant.o" -o "$out/vfs_${cmd}-$variant$suffix" -lpthread -lm
    echo "built $out/vfs_${cmd}-$variant$suffix"
    ;;
  micro)
    gcc -Wall -Wextra -std=gnu11 -O2 "$here/commit_micro.c" -o "$out/commit_micro"
    echo "built $out/commit_micro"
    ;;
  *) echo "usage: $0 sqlite | micro | test [stock|batch] | bench [stock|batch]" >&2; exit 2 ;;
esac
