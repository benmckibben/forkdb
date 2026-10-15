#!/usr/bin/env bash
# Put prebuilt libraries into the Python and Node package directories (the libraries are build output and are never committed).
#   scripts/stage-packages.sh python <library>               one library into python/forkdb/_lib/ (for the wheel of that platform)
#   scripts/stage-packages.sh node <artifacts-dir>           <artifacts-dir>/{linux-x86_64,darwin-arm64,windows-x86_64}/forkdb.{so,dylib,dll} into node/lib/{linux-x64,darwin-arm64,win32-x64}/
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
cp LICENSE-MIT LICENSE-APACHE python/ 2>/dev/null || cp LICENSE* python/
cp LICENSE-MIT LICENSE-APACHE node/ 2>/dev/null || cp LICENSE* node/
case "${1:-}" in
  python)
    lib=${2:?usage: stage-packages.sh python <library>}
    mkdir -p python/forkdb/_lib; rm -f python/forkdb/_lib/forkdb.*
    cp "$lib" "python/forkdb/_lib/$(basename "$lib")"
    echo "staged $lib into python/forkdb/_lib/" ;;
  node)
    dir=${2:?usage: stage-packages.sh node <artifacts-dir>}
    rm -rf node/lib
    for pair in linux-x86_64:linux-x64:so darwin-arm64:darwin-arm64:dylib windows-x86_64:win32-x64:dll; do
      IFS=: read -r from to ext <<< "$pair"
      f="$dir/$from/forkdb.$ext"
      [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
      mkdir -p "node/lib/$to"; cp "$f" "node/lib/$to/"
    done
    echo "staged node/lib: $(find node/lib -type f | sort | tr '\n' ' ')" ;;
  *) echo "usage: stage-packages.sh python <library> | node <artifacts-dir>" >&2; exit 2 ;;
esac
