#!/usr/bin/env bash
# Build the spike. Usage: build.sh [stock|batch]
#   stock  SQLite with the session extension (what a normal build can give you)
#   batch  stock + SQLITE_ENABLE_BATCH_ATOMIC_WRITE (exercises the batch-atomic path)
# Compiling the SQLite amalgamation takes several minutes at -O1; set OPT=-O0 for a quick build.
set -euo pipefail

variant=${1:-batch}
case $variant in
  stock) extra="" ;;
  batch) extra="-DSQLITE_ENABLE_BATCH_ATOMIC_WRITE" ;;
  *) echo "usage: $0 [stock|batch]" >&2; exit 2 ;;
esac
opt=${OPT:--O1}

here=$(cd "$(dirname "$0")" && pwd)
"$here/fetch-sqlite.sh"
src=$here/vendor/sqlite-amalgamation-3500000
out=$here/build
mkdir -p "$out"

flags="-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK $extra"

# shellcheck disable=SC2086  # $flags is a list of options on purpose
gcc $opt -c $flags "$src/sqlite3.c" -o "$out/sqlite3-$variant.o"
# shellcheck disable=SC2086
gcc -O2 -Wall -Wno-unused-function -Wno-misleading-indentation $flags -I"$src" -c "$here/spike.c" -o "$out/spike-$variant.o"
gcc "$out/spike-$variant.o" "$out/sqlite3-$variant.o" -o "$out/spike-$variant" -lpthread -ldl -lm
echo "built $out/spike-$variant"
echo "run it:  $out/spike-$variant"
