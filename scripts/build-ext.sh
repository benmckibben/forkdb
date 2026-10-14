#!/usr/bin/env bash
# Build the loadable extension: build/ext/forkdb.so (Linux, and macOS with .dylib naming left to the caller via FK_EXT_OUT).
# Needs gcc and the SQLite headers (spike/fetch-sqlite.sh fetches the pinned amalgamation; no SQLite is compiled).
#   scripts/build-ext.sh             build the portable extension, then check the result (see below)
# Portable means: no session driver (diff, promote and undo report that this host has none), every SQLite call goes through the host's
# sqlite3_api_routines table (ext/fk_ext_shim.h), and only the two init functions are exported.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
spike/fetch-sqlite.sh > /dev/null
src=spike/vendor/sqlite-amalgamation-3500000
out=${FK_EXT_OUT:-$root/build/ext}
mkdir -p "$out"
gcc -Wall -Wextra -std=gnu11 -O2 -g -fPIC -shared -fvisibility=hidden \
  -include ext/fk_ext_shim.h -I"$src" -Istore -Ivfs -Ifkx \
  ext/forkdb_ext.c fkx/fkx.c fkx/fkx_nosession.c vfs/fkvfs.c store/fkstore.c store/io_file.c store/io_prealloc.c \
  -o "$out/forkdb.so" -lpthread -lm
# The checks that make it loadable anywhere: nothing from SQLite left to resolve at load time, and the entry points are exported.
undef=$(nm -D --undefined-only "$out/forkdb.so" | grep -c 'sqlite3' || true)
if [ "$undef" != 0 ]; then
  echo "forkdb.so has $undef undefined sqlite3 symbols (it would only load where the host exports them):" >&2
  nm -D --undefined-only "$out/forkdb.so" | grep sqlite3 >&2
  exit 1
fi
for sym in sqlite3_forkdb_init sqlite3_extension_init; do
  nm -D --defined-only "$out/forkdb.so" | grep -q " T $sym\$" || { echo "$sym is not exported" >&2; exit 1; }
done
echo "built $out/forkdb.so"
