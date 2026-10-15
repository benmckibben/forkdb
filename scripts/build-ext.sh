#!/usr/bin/env bash
# Build the loadable extension for this platform: build/ext/forkdb.so (Linux), forkdb.dylib (macOS) or forkdb.dll (Windows, from Git Bash or MSYS2 with mingw-w64 gcc).
# Needs a C compiler (gcc, or CC=clang) and the SQLite headers (spike/fetch-sqlite.sh fetches the pinned amalgamation; no SQLite is compiled).
#   scripts/build-ext.sh             build the portable extension, then check the result (see below)
#   scripts/build-ext.sh --path      print where the library is (or will be) for this platform, and do nothing else
#   FK_EXT_OUT=dir                   build somewhere else
#   FK_EXT_STRIP=1                   strip the library (about 109 KB on Linux, instead of 458 KB with debug info) before the checks below, which then prove the two init functions are still exported
# Portable means: no session driver (diff, promote and undo report that this host has none), every SQLite call goes through the host's
# sqlite3_api_routines table (ext/fk_ext_shim.h), and only the two init functions are exported.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
case "$(uname -s)" in
  Linux) os=linux; ext=so ;;
  Darwin) os=macos; ext=dylib ;;
  MINGW* | MSYS* | CYGWIN*) os=windows; ext=dll ;;
  *) echo "build-ext.sh: unsupported platform $(uname -s)" >&2; exit 1 ;;
esac
out=${FK_EXT_OUT:-$root/build/ext}
lib=$out/forkdb.$ext
# On Windows the path is handed to native programs (node.exe, python.exe), which want C:/a/b, not /c/a/b.
shown=$lib
if [ "$os" = windows ] && command -v cygpath > /dev/null; then shown=$(cygpath -m "$lib"); fi
if [ "${1:-}" = "--path" ]; then echo "$shown"; exit 0; fi

spike/fetch-sqlite.sh > /dev/null
src=spike/vendor/sqlite-amalgamation-3500000
mkdir -p "$out"
cc=${CC:-gcc}
sources="ext/forkdb_ext.c fkx/fkx.c fkx/fkx_nosession.c vfs/fkvfs.c store/fkstore.c store/io_file.c store/io_prealloc.c"
flags="-Wall -Wextra -std=gnu11 -O2 -g -fvisibility=hidden -include ext/fk_ext_shim.h -I$src -Istore -Ivfs -Ifkx"
# shellcheck disable=SC2086
case $os in
  linux) $cc $flags -fPIC -shared $sources -o "$lib" -lpthread -lm -ldl ;;
  macos) $cc $flags -fPIC -dynamiclib $sources -o "$lib" -lpthread -lm ;;
  windows) $cc $flags -shared -static-libgcc $sources store/io_file_win.c -o "$lib" -lbcrypt ;;
esac

if [ -n "${FK_EXT_STRIP:-}" ]; then
  case $os in
    macos) strip -x "$lib" ;;
    *) strip --strip-unneeded "$lib" ;;
  esac
fi

# The checks that make it loadable anywhere: nothing from SQLite left to resolve at load time, and the entry points are exported.
case $os in
  linux)
    undefined=$(nm -D --undefined-only "$lib" | grep 'sqlite3' || true)
    exported=$(nm -D --defined-only "$lib" | grep -cE ' T (sqlite3_forkdb_init|sqlite3_extension_init)$' || true) ;;
  macos)
    undefined=$(nm -u "$lib" | grep 'sqlite3' || true)
    exported=$(nm -gU "$lib" | grep -cE ' T _(sqlite3_forkdb_init|sqlite3_extension_init)$' || true) ;;
  windows)
    undefined=$(objdump -p "$lib" | grep 'DLL Name' | grep -i sqlite || true) # a DLL that imports sqlite3.dll would only load where that DLL is around
    exported=$(objdump -p "$lib" | grep -cE '(^|[[:space:]])(sqlite3_forkdb_init|sqlite3_extension_init)$' || true) ;;
esac
if [ -n "$undefined" ]; then
  echo "forkdb.$ext has unresolved sqlite3 references (it would only load where the host exports them):" >&2
  echo "$undefined" >&2
  exit 1
fi
if [ "$exported" != 2 ]; then
  echo "forkdb.$ext should export sqlite3_forkdb_init and sqlite3_extension_init: found $exported of the 2" >&2
  exit 1
fi
echo "built $shown"
