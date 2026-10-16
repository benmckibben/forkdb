#!/usr/bin/env bash
# shellcheck disable=SC2086  # the flags and source lists are lists of words on purpose
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
VENDOR=$root/spike/vendor/sqlite-amalgamation-3500000
VFS_BUILD=$root/vfs/build
out=$root/build
mkdir -p "$out"
F="-Wall -Wextra -std=gnu11 -O1 -g"
# The release build of the CLI (.github/workflows/release.yml) sets these; without them the CLI is built as before, for the tests.
#   FK_CLI_OPT=-O2   the optimisation flags for the CLI and its shell (default: -O1 -g)
#   FK_CLI_OUT=dir   where the CLI binary goes (default: build/)
#   FK_CLI_STRIP=1   strip the binary
Fcli="-Wall -Wextra -std=gnu11 ${FK_CLI_OPT:--O1 -g}"
cliout=${FK_CLI_OUT:-$out}
FS="-DSQLITE_ENABLE_SESSION -DSQLITE_ENABLE_PREUPDATE_HOOK -DFK_MAX_BRANCHES=64"
I="-I$VENDOR -I$VFS_BUILD -I$root/store -I$root/vfs -I$root/fkx"
SESS_OBJ=$VFS_BUILD/sqlite3-session.o
SRC="$root/fkx/fkx.c $root/fkx/fkx_session.c $root/vfs/fkvfs.c $root/store/fkstore.c $root/store/io_file.c $root/store/io_prealloc.c"
[ -f "$SESS_OBJ" ] || { echo "run vfs/build.sh sqlite first" >&2; exit 1; }
cmd=${1:-}
case $cmd in
  cli)
    sed 's/^int SQLITE_CDECL main(/int SQLITE_CDECL shell_main(/' "$VENDOR/shell.c" > "$out/shell-renamed.c"
    # The same SQLite as the one in sqlite3-session.o: its headers, and without loadable extensions (that object is built without them)
    mkdir -p "$cliout"
    gcc $Fcli $FS -DSQLITE_OMIT_LOAD_EXTENSION -I"$VENDOR" -c "$out/shell-renamed.c" -o "$out/shell.o" 2>&1 | tail -1
    gcc $Fcli $FS $I $SRC "$root/cli/forkdb.c" "$SESS_OBJ" "$out/shell.o" -o "$cliout/forkdb" -lpthread -lm 2>&1 | tail -1 || {
      # show actual errors
      gcc $Fcli $FS $I $SRC "$root/cli/forkdb.c" "$SESS_OBJ" "$out/shell.o" -o "$cliout/forkdb" -lpthread -lm 2>&1 | grep error
      exit 1
    }
    if [ -n "${FK_CLI_STRIP:-}" ]; then
      case "$(uname -s)" in Darwin) strip -x "$cliout/forkdb" ;; *) strip --strip-unneeded "$cliout/forkdb" ;; esac
    fi
    echo "built $cliout/forkdb" ;;
  ext)
    gcc $F $FS $I -fPIC -shared "$root/ext/forkdb_ext.c" $SRC -o "$out/forkdb.so" -lpthread -lm 2>&1 | tail -1 || {
      gcc $F $FS $I -fPIC -shared "$root/ext/forkdb_ext.c" $SRC -o "$out/forkdb.so" -lpthread -lm 2>&1 | grep error
      exit 1
    }
    echo "built $out/forkdb.so" ;;
  test)
    "$0" cli; "$0" ext
    # The smoke test of the CLI is test/smoke_cli.sh (the release workflow runs it on the built binary too). It exits non-zero on the first failed check.
    version=$(grep -o 'define FORKDB_VERSION "[0-9][0-9.]*"' "$root/cli/forkdb.c" | grep -o '[0-9][0-9.]*')
    "$root/test/smoke_cli.sh" "$out/forkdb" "$version"
    echo "the extension is tested by scripts/test-ext.sh, scripts/test-ext-node.sh and scripts/test-ext-bsq.sh"
    echo "END rc=0" ;;
  *) echo "usage: $0 cli | ext | test" >&2; exit 2 ;;
esac
