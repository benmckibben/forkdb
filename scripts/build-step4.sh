#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
VENDOR=$root/spike/vendor/sqlite-amalgamation-3500000
VFS_BUILD=$root/vfs/build
out=$root/build
mkdir -p "$out"
F="-Wall -Wextra -std=gnu11 -O1 -g"
FS="-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DFK_MAX_BRANCHES=64"
I="-I$VENDOR -I$VFS_BUILD -I$root/store -I$root/vfs -I$root/fkx"
SESS_OBJ=$VFS_BUILD/sqlite3-session.o
SRC="$root/fkx/fkx.c $root/fkx/fkx_session.c $root/vfs/fkvfs.c $root/store/fkstore.c $root/store/io_file.c $root/store/io_prealloc.c"
[ -f "$SESS_OBJ" ] || { echo "run vfs/build.sh sqlite first" >&2; exit 1; }
cmd=${1:-}
case $cmd in
  cli)
    sed 's/^int SQLITE_CDECL main(/int SQLITE_CDECL shell_main(/' "$VENDOR/shell.c" > "$out/shell-renamed.c"
    gcc $F $FS -c "$out/shell-renamed.c" -o "$out/shell.o" 2>&1 | tail -1
    gcc $F $FS $I $SRC "$root/cli/forkdb.c" "$SESS_OBJ" "$out/shell.o" -o "$out/forkdb" -lpthread -lm 2>&1 | tail -1 || {
      # show actual errors
      gcc $F $FS $I $SRC "$root/cli/forkdb.c" "$SESS_OBJ" "$out/shell.o" -o "$out/forkdb" -lpthread -lm 2>&1 | grep error
      exit 1
    }
    echo "built $out/forkdb" ;;
  ext)
    gcc $F $FS $I -fPIC -shared "$root/ext/forkdb_ext.c" $SRC -o "$out/forkdb.so" -lpthread -lm 2>&1 | tail -1 || {
      gcc $F $FS $I -fPIC -shared "$root/ext/forkdb_ext.c" $SRC -o "$out/forkdb.so" -lpthread -lm 2>&1 | grep error
      exit 1
    }
    echo "built $out/forkdb.so" ;;
  test)
    "$0" cli; "$0" ext
    store=$(mktemp -u /tmp/fk4-smoke-XXXX.fk)
    echo "=== CLI ==="
    "$out/forkdb" init "$store" && "$out/forkdb" fork "$store" feat-x && "$out/forkdb" list "$store" | grep -c main && "$out/forkdb" info "$store" feat-x | grep size_pages && "$out/forkdb" diff "$store" feat-x | grep changeset && "$out/forkdb" promote "$store" feat-x --dry-run | grep promoted && "$out/forkdb" promote "$store" feat-x | grep promoted && "$out/forkdb" undo "$store" main | grep undone && "$out/forkdb" snapshot "$store" snap1 && "$out/forkdb" discard "$store" feat-x && "$out/forkdb" gc "$store" | grep discarded && "$out/forkdb" compact "$store" | grep compacted && echo "CLI PASSED"
    rm -f "$store"
    echo "=== Extension ==="
    gcc $F $FS $I "$root/test/ext_smoke_test.c" $SRC "$SESS_OBJ" -o "$out/ext_smoke" -lpthread -lm -ldl
    "$out/ext_smoke" "$out/forkdb.so"
    echo "EXTENSION PASSED"
    echo "END rc=0" ;;
  *) echo "usage: $0 cli | ext | test" >&2; exit 2 ;;
esac
